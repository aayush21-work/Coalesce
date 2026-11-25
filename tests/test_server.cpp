#include<gtest/gtest.h>
#include<nlohmann/json.hpp>
#include"helpers.hpp"
#include"server.hpp"

using json = nlohmann::json;

struct Server_test : testing::Test
{
    Test_dir dir;
    Store store{dir.db()};
    Counter counter{store};
    Server server{counter, store, dir.path};
    int port = server.svr.bind_to_any_port("127.0.0.1");
    std::jthread listener{[this] { server.svr.listen_after_bind(); }};
    httplib::Client client{"127.0.0.1", port};

    Server_test() { server.svr.wait_until_ready(); }
    ~Server_test() override { server.stop(); }
};

TEST_F(Server_test, like_dislike_current)
{
    EXPECT_EQ(client.Post("/like")->body, "{\"count\":1}");
    EXPECT_EQ(client.Post("/like")->body, "{\"count\":2}");
    EXPECT_EQ(client.Post("/dislike")->body, "{\"count\":1}");
    EXPECT_EQ(client.Get("/current")->body, "{\"count\":1}");
}

TEST_F(Server_test, concurrent_likes_are_exact)
{
    run_threads(8, [&](int) {
        httplib::Client c("127.0.0.1", port);
        c.set_keep_alive(true);
        for(int k = 0; k < 100; k++)
        {
            auto res = c.Post("/like");
            ASSERT_TRUE(res);
            ASSERT_EQ(res->status, 200);
        }
    });
    EXPECT_TRUE(counter.try_flush());
    EXPECT_EQ(store.read(), 800);
}

TEST_F(Server_test, strategy_switch)
{
    auto ok = client.Put("/api/strategy", "{\"strategy\":\"sharded_batch\"}", "application/json");
    EXPECT_EQ(ok->status, 200);
    EXPECT_EQ(counter.active_name(), "sharded_batch");

    EXPECT_EQ(client.Put("/api/strategy", "{\"strategy\":\"racy\"}", "application/json")->status, 400);
    EXPECT_EQ(client.Put("/api/strategy", "{\"strategy\":5}", "application/json")->status, 400);
    EXPECT_EQ(client.Put("/api/strategy", "not json", "application/json")->status, 400);
    EXPECT_EQ(client.Put("/api/strategy", "{\"strategy\":\"mutex_batch\"}", "text/plain")->status, 415);
    EXPECT_EQ(counter.active_name(), "sharded_batch");
}

TEST_F(Server_test, bench_checks_input)
{
    for(std::string body : {"{\"threads\":0}", "{\"threads\":65}", "{\"threads\":\"8\"}", "{\"seconds\":11}",
                            "{\"seconds\":-1}", "{\"seconds\":18446744073709551615}", "{\"strategy\":\"x\"}",
                            "{\"strategy\":1}", "[]"})
    {
        EXPECT_EQ(client.Post("/api/bench", body, "application/json")->status, 400) << body;
    }
    EXPECT_EQ(client.Post("/api/bench", "{}", "text/plain")->status, 415);
}

TEST_F(Server_test, bench_runs_without_touching_the_live_count)
{
    std::string body = "{\"strategy\":\"atomic_batch\",\"threads\":2,\"seconds\":1}";
    EXPECT_EQ(client.Post("/api/bench", body, "application/json")->status, 202);
    EXPECT_EQ(client.Post("/api/bench", body, "application/json")->status, 409);

    json state;
    for(int i = 0; i < 100; i++)
    {
        state = json::parse(client.Get("/api/bench")->body);
        if(!state["running"].get<bool>()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_EQ(state["results"].size(), 1u);
    EXPECT_EQ(state["results"][0]["lost"], 0);
    EXPECT_EQ(client.Get("/current")->body, "{\"count\":0}");
}

TEST_F(Server_test, stream_sends_history_then_samples)
{
    std::string received;
    client.Get("/api/stream", [&](const char *data, std::size_t size) {
        received.append(data, size);
        return received.size() < 2000;
    });
    EXPECT_EQ(received.rfind("event: history\n", 0), 0u);
    EXPECT_NE(received.find("\"durable\""), std::string::npos);
}

TEST_F(Server_test, second_server_cannot_share_the_port)
{
    Server second(counter, store, dir.path);
    EXPECT_FALSE(second.svr.bind_to_port("127.0.0.1", port));
}

TEST_F(Server_test, serves_dashboard)
{
    EXPECT_EQ(client.Get("/")->status, 200);
    EXPECT_EQ(client.Get("/app.js")->status, 200);
    EXPECT_EQ(client.Get("/style.css")->status, 200);
    EXPECT_EQ(client.Get("/nothing")->status, 404);
}

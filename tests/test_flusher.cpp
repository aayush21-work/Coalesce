#include<gtest/gtest.h>
#include<stdexcept>
#include"flusher.hpp"
#include"helpers.hpp"

TEST(Flush, writes_only_the_difference)
{
    Test_dir dir;
    Store store(dir.db());
    auto s = make_strategy("atomic_batch", store);
    s->add(10);
    flush({s.get()}, [&](std::int64_t d) { store.add(d); });
    s->add(3);
    flush({s.get()}, [&](std::int64_t d) { store.add(d); });
    EXPECT_EQ(store.read(), 13);
    EXPECT_EQ(store.writes, 2u);
}

TEST(Flush, idle_does_not_write)
{
    Test_dir dir;
    Store store(dir.db());
    Counter counter(store);
    EXPECT_TRUE(counter.try_flush());
    EXPECT_EQ(store.writes, 0u);
}

TEST(Flush, failed_write_is_retried)
{
    Test_dir dir;
    Store store(dir.db());
    auto s = make_strategy("sharded_batch", store);
    s->add(7);
    EXPECT_THROW(flush({s.get()}, [](std::int64_t) { throw std::runtime_error("disk full"); }), std::runtime_error);
    EXPECT_EQ(s->last_written, 0);
    s->add(1);
    flush({s.get()}, [&](std::int64_t d) { store.add(d); });
    EXPECT_EQ(store.read(), 8);
}

TEST(Counter, count_survives_restart)
{
    Test_dir dir;
    {
        Store store(dir.db());
        Counter counter(store);
        counter.add(41);
        EXPECT_TRUE(counter.try_flush());
    }
    Store store(dir.db());
    Counter counter(store);
    counter.add(1);
    EXPECT_EQ(counter.accepted(), 42);
    EXPECT_EQ(counter.durable(), 41);
}

TEST(Counter, rejects_racy_and_unknown)
{
    Test_dir dir;
    Store store(dir.db());
    Counter counter(store);
    EXPECT_FALSE(counter.set_active("racy"));
    EXPECT_FALSE(counter.set_active(""));
    EXPECT_EQ(counter.active_name(), "atomic_batch");
}

TEST(Counter, switching_under_load_loses_nothing)
{
    Test_dir dir;
    Store store(dir.db());
    Counter counter(store);
    {
        std::jthread flusher(update_count, std::ref(counter), std::chrono::milliseconds(1));
        std::jthread switcher([&](std::stop_token stop) {
            for(std::size_t i = 0; !stop.stop_requested(); i++)
            {
                counter.set_active(live_strategies()[i % live_strategies().size()]);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        run_threads(8, [&](int) {
            for(int k = 0; k < 2000; k++) counter.add(1);
        });
        switcher.request_stop();
        switcher.join();
    }
    // the flusher has been joined, so its final flush is done
    EXPECT_EQ(counter.accepted(), 16000);
    EXPECT_EQ(counter.durable(), 16000);
    EXPECT_EQ(store.read(), 16000);
}

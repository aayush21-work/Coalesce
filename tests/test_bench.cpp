#include<gtest/gtest.h>
#include"bench.hpp"
#include"helpers.hpp"
#include"strategy.hpp"

static Bench_result quick_run(const std::string &name, const Test_dir &dir)
{
    Bench_config config;
    config.threads = 4;
    config.seconds = 1;
    return run_bench(name, config, dir.path, {}, [](double, std::uint64_t) {});
}

TEST(Bench, safe_strategies_lose_nothing)
{
    Test_dir dir;
    for(const std::string &name : live_strategies())
    {
        Bench_result r = quick_run(name, dir);
        EXPECT_GT(r.ops, 0u) << name;
        EXPECT_EQ(r.lost, 0) << name;
        EXPECT_TRUE(r.p50.has_value()) << name;
    }
    EXPECT_TRUE(std::filesystem::is_empty(dir.path)); // cleaned up
}

TEST(Bench, batching_needs_few_writes)
{
    Test_dir dir;
    Bench_result r = quick_run("atomic_batch", dir);
    EXPECT_LE(r.db_writes, 20u);
}

TEST(Bench, one_at_a_time_and_stoppable)
{
    Test_dir dir;
    Bench bench(dir.path);
    Bench_config config;
    config.strategy = "atomic_batch";
    config.threads = 1;
    config.seconds = 10;
    EXPECT_TRUE(bench.start(config));
    EXPECT_FALSE(bench.start(config));
    auto before = std::chrono::steady_clock::now();
    bench.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(5));
    EXPECT_FALSE(bench.state()["running"]);
}

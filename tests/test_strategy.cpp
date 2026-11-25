#include<gtest/gtest.h>
#include"helpers.hpp"
#include"strategy.hpp"

struct Safe_strategy : testing::TestWithParam<std::string>
{
};

TEST_P(Safe_strategy, exact_total_under_contention)
{
    Test_dir dir;
    Store store(dir.db());
    auto strategy = make_strategy(GetParam(), store);
    int per_thread = strategy->batched() ? 20000 : 100;

    run_threads(8, [&](int i) {
        for(int k = 0; k < per_thread; k++) strategy->add(i % 2 == 0 ? 2 : -1);
    });
    EXPECT_EQ(strategy->total(), 4 * per_thread * 2 - 4 * per_thread);
}

INSTANTIATE_TEST_SUITE_P(all, Safe_strategy, testing::ValuesIn(live_strategies()),
                         [](const auto &info) { return info.param; });

TEST(Strategy, write_through_is_durable_on_return)
{
    Test_dir dir;
    Store store(dir.db());
    Store other(dir.db());
    auto strategy = make_strategy("write_through", store);
    strategy->add(5);
    EXPECT_EQ(other.read(), 5);
}

TEST(Strategy, unknown_name_gives_null)
{
    Test_dir dir;
    Store store(dir.db());
    EXPECT_EQ(make_strategy("nope", store), nullptr);
}

TEST(Store, two_connections_add_up)
{
    Test_dir dir;
    Store a(dir.db());
    Store b(dir.db());
    run_threads(2, [&](int i) {
        for(int k = 0; k < 100; k++) (i == 0 ? a : b).add(1);
    });
    EXPECT_EQ(a.read(), 200);
}

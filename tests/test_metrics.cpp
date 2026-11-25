#include<gtest/gtest.h>
#include<limits>
#include"metrics.hpp"

TEST(Histogram, every_value_is_inside_its_bucket)
{
    for(std::uint64_t v : {0ull, 1ull, 15ull, 16ull, 31ull, 32ull, 1000ull, 123456789ull, ~0ull})
    {
        std::size_t b = histogram::bucket(v);
        ASSERT_LT(b, histogram::size);
        EXPECT_GE(histogram::upper_bound(b), v);
        if(b > 0)
        {
            EXPECT_LT(histogram::upper_bound(b - 1), v);
        }
    }
}

TEST(Histogram, percentiles_are_within_7_percent)
{
    std::vector<std::uint64_t> counts(histogram::size);
    for(int v = 1; v <= 100000; v++) counts[histogram::bucket(v)]++;
    EXPECT_NEAR(*histogram::percentile(counts, 0.50), 50000, 50000 * 0.07);
    EXPECT_NEAR(*histogram::percentile(counts, 0.99), 99000, 99000 * 0.07);
}

TEST(Histogram, empty_has_no_percentile)
{
    EXPECT_FALSE(histogram::percentile(std::vector<std::uint64_t>(histogram::size), 0.5).has_value());
}

TEST(Metrics, records)
{
    Metrics metrics;
    metrics.record(100);
    metrics.record(100);
    EXPECT_EQ(metrics.requests, 2u);
    EXPECT_EQ(metrics.counts()[histogram::bucket(100)], 2u);
}

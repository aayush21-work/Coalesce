#pragma once
#include<array>
#include<atomic>
#include<cstddef>
#include<cstdint>
#include<optional>
#include<vector>

// latency histogram with log spaced buckets: 16 steps per power of two,
// so every value is known to about 6 percent using a fixed amount of memory
namespace histogram
{
    constexpr std::size_t size = 976;
    std::size_t bucket(std::uint64_t ns);
    std::uint64_t upper_bound(std::size_t bucket); // biggest value in a bucket
    // p between 0 and 1. empty if there are no samples
    std::optional<std::uint64_t> percentile(const std::vector<std::uint64_t> &counts, double p);
}

// live request metrics, written by the http threads
struct Metrics
{
    void record(std::uint64_t ns);
    std::vector<std::uint64_t> counts() const;

    std::atomic<std::uint64_t> requests = 0;

private:
    std::array<std::atomic<std::uint64_t>, histogram::size> buckets{};
};

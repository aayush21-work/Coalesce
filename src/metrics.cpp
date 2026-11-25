#include"metrics.hpp"
#include<bit>
#include<cmath>

namespace histogram
{
    std::size_t bucket(std::uint64_t ns)
    {
        if(ns < 16) return ns; // small values are exact
        std::size_t msb = 63 - std::countl_zero(ns);
        std::size_t step = (ns >> (msb - 4)) & 15;
        return 16 + (msb - 4) * 16 + step;
    }

    std::uint64_t upper_bound(std::size_t bucket)
    {
        if(bucket < 16) return bucket;
        std::size_t shift = (bucket - 16) / 16;
        std::uint64_t low = 16 + (bucket - 16) % 16;
        return (low << shift) + ((std::uint64_t{1} << shift) - 1);
    }

    std::optional<std::uint64_t> percentile(const std::vector<std::uint64_t> &counts, double p)
    {
        std::uint64_t total = 0;
        for(std::uint64_t c : counts) total += c;
        if(total == 0) return std::nullopt;

        std::uint64_t rank = static_cast<std::uint64_t>(std::ceil(p * total));
        if(rank == 0) rank = 1;
        std::uint64_t seen = 0;
        for(std::size_t i = 0; i < counts.size(); i++)
        {
            seen += counts[i];
            if(seen >= rank) return upper_bound(i);
        }
        return upper_bound(counts.size() - 1);
    }
}

void Metrics::record(std::uint64_t ns)
{
    requests.fetch_add(1, std::memory_order_relaxed);
    buckets[histogram::bucket(ns)].fetch_add(1, std::memory_order_relaxed);
}

std::vector<std::uint64_t> Metrics::counts() const
{
    std::vector<std::uint64_t> copy(histogram::size);
    for(std::size_t i = 0; i < copy.size(); i++)
    {
        copy[i] = buckets[i].load(std::memory_order_relaxed);
    }
    return copy;
}

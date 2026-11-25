#pragma once
#include<array>
#include<atomic>
#include<cstddef>
#include<cstdint>
#include<memory>
#include<mutex>
#include<string>
#include<vector>
#include"store.hpp"

// a strategy is one way of accepting likes from many threads.
// total() is a running total that never resets. the flusher remembers how
// much of it is already in the database (last_written) and writes the rest.
struct Strategy
{
    virtual ~Strategy() = default;
    virtual void add(std::int64_t delta) = 0;
    virtual std::int64_t total() const = 0;
    virtual bool batched() const { return true; }

    std::atomic<std::int64_t> last_written = 0;
};

// every like is its own UPDATE
struct Write_through : Strategy
{
    explicit Write_through(Store &s) : store(s) {}
    void add(std::int64_t delta) override;
    std::int64_t total() const override;
    bool batched() const override { return false; }

private:
    Store &store;
};

struct Mutex_batch : Strategy
{
    void add(std::int64_t delta) override;
    std::int64_t total() const override;

private:
    mutable std::mutex m;
    std::int64_t count = 0;
};

// the design from the original hit_counter.cpp
struct Atomic_batch : Strategy
{
    void add(std::int64_t delta) override;
    std::int64_t total() const override;

private:
    std::atomic<std::int64_t> count = 0;
};

// each thread gets its own cache line, so cores stop fighting over one
struct Sharded_batch : Strategy
{
    void add(std::int64_t delta) override;
    std::int64_t total() const override;

private:
    static constexpr std::size_t shard_count = 64;
    struct alignas(64) Shard
    {
        std::atomic<std::int64_t> count = 0;
    };
    std::array<Shard, shard_count> shards;
};

// wrong on purpose: load and store are separate steps, so updates get lost.
// relaxed atomics keep it free of undefined behaviour.
struct Racy : Strategy
{
    void add(std::int64_t delta) override;
    std::int64_t total() const override;

private:
    std::atomic<std::int64_t> count = 0;
};

const std::vector<std::string> &live_strategies();
const std::vector<std::string> &bench_strategies(); // live ones plus racy

// returns nullptr for an unknown name
std::unique_ptr<Strategy> make_strategy(const std::string &name, Store &store);

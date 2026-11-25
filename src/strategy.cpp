#include"strategy.hpp"

void Write_through::add(std::int64_t delta)
{
    store.add(delta); // throws before anything is counted
    last_written += delta;
}

std::int64_t Write_through::total() const
{
    return last_written.load();
}

void Mutex_batch::add(std::int64_t delta)
{
    std::lock_guard<std::mutex> lock(m);
    count += delta;
}

std::int64_t Mutex_batch::total() const
{
    std::lock_guard<std::mutex> lock(m);
    return count;
}

void Atomic_batch::add(std::int64_t delta)
{
    count += delta;
}

std::int64_t Atomic_batch::total() const
{
    return count.load();
}

static std::size_t shard_id()
{
    static std::atomic<std::size_t> next_id = 0;
    thread_local std::size_t id = next_id++;
    return id;
}

void Sharded_batch::add(std::int64_t delta)
{
    shards[shard_id() % shard_count].count.fetch_add(delta, std::memory_order_relaxed);
}

std::int64_t Sharded_batch::total() const
{
    std::int64_t sum = 0;
    for(const Shard &shard : shards)
    {
        sum += shard.count.load(std::memory_order_relaxed);
    }
    return sum;
}

void Racy::add(std::int64_t delta)
{
    std::int64_t old = count.load(std::memory_order_relaxed);
    count.store(old + delta, std::memory_order_relaxed);
}

std::int64_t Racy::total() const
{
    return count.load();
}

const std::vector<std::string> &live_strategies()
{
    static const std::vector<std::string> names = {
        "write_through", "mutex_batch", "atomic_batch", "sharded_batch"};
    return names;
}

const std::vector<std::string> &bench_strategies()
{
    static const std::vector<std::string> names = {
        "write_through", "mutex_batch", "atomic_batch", "sharded_batch", "racy"};
    return names;
}

std::unique_ptr<Strategy> make_strategy(const std::string &name, Store &store)
{
    if(name == "write_through") return std::make_unique<Write_through>(store);
    if(name == "mutex_batch") return std::make_unique<Mutex_batch>();
    if(name == "atomic_batch") return std::make_unique<Atomic_batch>();
    if(name == "sharded_batch") return std::make_unique<Sharded_batch>();
    if(name == "racy") return std::make_unique<Racy>();
    return nullptr;
}

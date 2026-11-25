#include"bench.hpp"
#include<atomic>
#include<condition_variable>
#include<latch>
#include<stdexcept>
#include<stdlib.h>
#include"flusher.hpp"
#include"metrics.hpp"
#include"store.hpp"
#include"strategy.hpp"

using bench_clock = std::chrono::steady_clock;

// private folder for the benchmark database, removed when the run ends
struct Temp_dir
{
    std::filesystem::path path;

    explicit Temp_dir(const std::filesystem::path &parent)
    {
        std::string name = (parent / "coalesce_bench_XXXXXX").string();
        if(!mkdtemp(name.data())) throw std::runtime_error("cannot create benchmark folder in " + parent.string());
        path = name;
    }
    Temp_dir(const Temp_dir &) = delete;
    ~Temp_dir()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

struct alignas(64) Worker_state
{
    std::atomic<std::uint64_t> ops = 0;
    std::vector<std::uint64_t> latency = std::vector<std::uint64_t>(histogram::size);
};

std::uint64_t clock_cost()
{
    // median of many back to back reads, measured once
    static const std::uint64_t cost = [] {
        std::vector<std::uint64_t> counts(histogram::size);
        for(int i = 0; i < 200000; i++)
        {
            auto before = bench_clock::now();
            auto after = bench_clock::now();
            counts[histogram::bucket(std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count())]++;
        }
        return histogram::percentile(counts, 0.50).value_or(0);
    }();
    return cost;
}

static std::uint64_t sum_ops(const std::vector<Worker_state> &states)
{
    std::uint64_t sum = 0;
    for(const Worker_state &s : states) sum += s.ops.load(std::memory_order_relaxed);
    return sum;
}

Bench_result run_bench(const std::string &strategy_name, const Bench_config &config,
                       const std::filesystem::path &dir, std::stop_token stop,
                       const std::function<void(double, std::uint64_t)> &progress)
{
    Temp_dir temp(dir);
    Store store((temp.path / "bench.db").string());
    std::unique_ptr<Strategy> strategy = make_strategy(strategy_name, store);
    if(!strategy) throw std::invalid_argument("unknown strategy " + strategy_name);

    // reading the clock costs more than an atomic add, so only time some ops
    const std::uint64_t sample_mask = strategy->batched() ? 63 : 0;

    std::vector<Worker_state> states(config.threads);
    std::atomic<bool> done = false;
    std::atomic<bool> failed = false;
    std::latch ready(config.threads + 1);
    std::vector<std::jthread> workers;

    for(int i = 0; i < config.threads; i++)
    {
        workers.emplace_back([&, i] {
            Worker_state &me = states[i];
            std::uint64_t ops = 0;
            ready.arrive_and_wait();
            try
            {
                while(!done.load(std::memory_order_relaxed))
                {
                    if((ops & sample_mask) == 0)
                    {
                        auto before = bench_clock::now();
                        strategy->add(1);
                        auto after = bench_clock::now();
                        std::uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
                        me.latency[histogram::bucket(ns)]++;
                    }
                    else
                    {
                        strategy->add(1);
                    }
                    ops++;
                    me.ops.store(ops, std::memory_order_relaxed);
                }
            }
            catch(const std::exception &)
            {
                failed = true;
            }
        });
    }

    std::jthread flusher;
    if(strategy->batched())
    {
        flusher = std::jthread([&](std::stop_token flusher_stop) {
            std::mutex wait_m;
            std::condition_variable_any cv;
            while(!flusher_stop.stop_requested())
            {
                std::unique_lock<std::mutex> lock(wait_m);
                cv.wait_for(lock, flusher_stop, config.flush_interval, [] { return false; });
                try
                {
                    flush({strategy.get()}, [&](std::int64_t delta) { store.add(delta); });
                }
                catch(const SQLite::Exception &)
                {
                    // retried on the next tick
                }
            }
        });
    }

    ready.arrive_and_wait();
    auto start = bench_clock::now();
    auto deadline = start + std::chrono::seconds(config.seconds);
    while(bench_clock::now() < deadline && !stop.stop_requested() && !failed)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        progress(std::chrono::duration<double>(bench_clock::now() - start).count(), sum_ops(states));
    }
    done = true;
    workers.clear(); // joins
    double elapsed = std::chrono::duration<double>(bench_clock::now() - start).count();

    if(flusher.joinable())
    {
        flusher.request_stop();
        flusher.join();
        flush({strategy.get()}, [&](std::int64_t delta) { store.add(delta); });
    }
    if(failed) throw std::runtime_error("database write failed during the benchmark");

    Bench_result r;
    r.strategy = strategy_name;
    r.threads = config.threads;
    r.seconds = elapsed;
    r.ops = sum_ops(states);
    r.ops_per_second = r.ops / elapsed;
    r.db_writes = store.writes;
    r.in_db = store.read(); // what really got saved
    r.lost = static_cast<std::int64_t>(r.ops) - r.in_db;

    std::vector<std::uint64_t> merged(histogram::size);
    for(const Worker_state &s : states)
    {
        for(std::size_t i = 0; i < merged.size(); i++) merged[i] += s.latency[i];
    }
    r.p50 = histogram::percentile(merged, 0.50);
    r.p99 = histogram::percentile(merged, 0.99);
    r.p999 = histogram::percentile(merged, 0.999);
    r.max = histogram::percentile(merged, 1.0);
    return r;
}

bool Bench::start(const Bench_config &config)
{
    std::lock_guard<std::mutex> lock(m);
    if(running) return false;
    running = true;
    current = config;
    current_strategy.clear();
    current_seconds = 0;
    current_ops = 0;
    error.clear();
    results.clear();
    if(worker.joinable()) worker.join(); // finished already, running was false
    worker = std::jthread([this, config](std::stop_token stop) { run(stop, config); });
    return true;
}

void Bench::run(std::stop_token stop, Bench_config config)
{
    std::vector<std::string> names = {config.strategy};
    if(config.strategy == "all") names = bench_strategies();

    try
    {
        for(const std::string &name : names)
        {
            if(stop.stop_requested()) break;
            {
                std::lock_guard<std::mutex> lock(m);
                current_strategy = name;
                current_seconds = 0;
                current_ops = 0;
            }
            Bench_result r = run_bench(name, config, dir, stop, [this](double seconds, std::uint64_t ops) {
                std::lock_guard<std::mutex> lock(m);
                current_seconds = seconds;
                current_ops = ops;
            });
            std::lock_guard<std::mutex> lock(m);
            results.push_back(r);
        }
    }
    catch(const std::exception &e)
    {
        std::lock_guard<std::mutex> lock(m);
        error = e.what();
    }
    std::lock_guard<std::mutex> lock(m);
    running = false;
}

static nlohmann::json or_null(const std::optional<std::uint64_t> &value)
{
    if(!value) return nullptr;
    return *value;
}

nlohmann::json Bench::state()
{
    std::lock_guard<std::mutex> lock(m);
    std::size_t total = current.strategy == "all" ? bench_strategies().size() : 1;
    nlohmann::json j = {
        {"running", running},
        {"threads", current.threads},
        {"seconds", current.seconds},
        {"clock_ns", clock_cost()},
        {"done", results.size()},
        {"total", total},
        {"results", nlohmann::json::array()}};
    if(running)
    {
        j["current"] = {
            {"strategy", current_strategy},
            {"elapsed", current_seconds},
            {"ops", current_ops},
            {"ops_per_second", current_seconds > 0 ? current_ops / current_seconds : 0.0}};
    }
    if(!error.empty()) j["error"] = error;
    for(const Bench_result &r : results)
    {
        j["results"].push_back({
            {"strategy", r.strategy},
            {"threads", r.threads},
            {"seconds", r.seconds},
            {"ops", r.ops},
            {"ops_per_second", r.ops_per_second},
            {"p50_ns", or_null(r.p50)},
            {"p99_ns", or_null(r.p99)},
            {"p999_ns", or_null(r.p999)},
            {"max_ns", or_null(r.max)},
            {"db_writes", r.db_writes},
            {"in_db", r.in_db},
            {"lost", r.lost}});
    }
    return j;
}

void Bench::stop()
{
    worker.request_stop();
    if(worker.joinable()) worker.join();
}

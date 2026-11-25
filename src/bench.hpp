#pragma once
#include<chrono>
#include<cstdint>
#include<filesystem>
#include<functional>
#include<mutex>
#include<optional>
#include<stop_token>
#include<string>
#include<thread>
#include<vector>
#include<nlohmann/json.hpp>

struct Bench_config
{
    std::string strategy = "all"; // a strategy name or "all"
    int threads = 8;
    int seconds = 2;
    std::chrono::milliseconds flush_interval{100};
};

struct Bench_result
{
    std::string strategy;
    int threads = 0;
    double seconds = 0;
    std::uint64_t ops = 0;
    double ops_per_second = 0;
    std::optional<std::uint64_t> p50, p99, p999, max; // nanoseconds
    std::uint64_t db_writes = 0;
    std::int64_t in_db = 0;
    std::int64_t lost = 0; // ops that never reached the database
};

// cost of reading the clock itself, included in every latency number
std::uint64_t clock_cost();

// closed loop load generator: every thread calls add(1) as fast as it can.
// runs against a throwaway database inside dir, never the live one.
// progress is called about 10 times a second with the ops so far
Bench_result run_bench(const std::string &strategy, const Bench_config &config,
                       const std::filesystem::path &dir, std::stop_token stop,
                       const std::function<void(double, std::uint64_t)> &progress);

// runs one benchmark at a time in the background
struct Bench
{
    explicit Bench(std::filesystem::path d) : dir(std::move(d)) {}

    bool start(const Bench_config &config); // false if one is already running
    nlohmann::json state();
    void stop();

private:
    void run(std::stop_token stop, Bench_config config);

    std::filesystem::path dir;
    std::mutex m;
    bool running = false;
    Bench_config current;
    std::string current_strategy;
    double current_seconds = 0;
    std::uint64_t current_ops = 0;
    std::string error;
    std::vector<Bench_result> results;
    std::jthread worker;
};

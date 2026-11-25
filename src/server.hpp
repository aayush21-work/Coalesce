#pragma once
#include<atomic>
#include<condition_variable>
#include<deque>
#include<filesystem>
#include<mutex>
#include<stop_token>
#include<string>
#include<thread>
#include<vector>
#include"httplib.h"
#include"bench.hpp"
#include"flusher.hpp"
#include"metrics.hpp"
#include"store.hpp"

struct Server
{
    Server(Counter &c, Store &s, const std::filesystem::path &bench_dir);
    ~Server();

    void stop(); // safe from any thread

    httplib::Server svr;

private:
    void add_routes();
    void change(std::int64_t delta, httplib::Response &res);
    void stream(const httplib::Request &req, httplib::Response &res);
    void sample(std::stop_token stop);

    Counter &counter;
    Store &store;
    Metrics metrics;
    Bench bench;

    // newest sample for the live stream, guarded by m
    std::mutex m;
    std::condition_variable cv;
    std::uint64_t sequence = 0;
    std::string latest;
    std::deque<std::string> history; // last minute of chart points

    std::atomic<int> streams = 0;
    std::atomic<bool> stopping = false;
    std::jthread sampler;
};

#pragma once
#include<atomic>
#include<chrono>
#include<cstdint>
#include<functional>
#include<memory>
#include<mutex>
#include<stop_token>
#include<string>
#include<vector>
#include"store.hpp"
#include"strategy.hpp"

// writes everything not yet in the database as one UPDATE.
// last_written only moves after the write succeeded, so a failed flush
// is simply retried with a bigger delta next time.
void flush(const std::vector<Strategy *> &strategies, const std::function<void(std::int64_t)> &write);

// the live counter: all strategies side by side, one of them active
struct Counter
{
    explicit Counter(Store &s);

    void add(std::int64_t delta);
    std::int64_t accepted() const; // what users see
    std::int64_t durable() const;  // what is in the database

    bool set_active(const std::string &name);
    std::string active_name() const;

    bool try_flush(); // false if the database refused

    std::atomic<std::uint64_t> flush_errors = 0;

private:
    Store &store;
    std::int64_t start_value;
    std::vector<std::unique_ptr<Strategy>> strategies;
    std::vector<Strategy *> batched;
    std::atomic<std::size_t> active;
    std::mutex flush_lock;
};

// flusher thread: flush, sleep, repeat. flushes one last time when stopped
void update_count(std::stop_token stop, Counter &counter, std::chrono::milliseconds interval);

#pragma once
#include<atomic>
#include<cstdint>
#include<mutex>
#include<string>
#include<SQLiteCpp/SQLiteCpp.h>

// the only code that touches sqlite
struct Store
{
    explicit Store(const std::string &path);

    std::int64_t read();
    void add(std::int64_t delta); // throws SQLite::Exception, nothing is written in that case

    std::atomic<std::uint64_t> writes = 0;

private:
    std::mutex m;
    SQLite::Database db;
};

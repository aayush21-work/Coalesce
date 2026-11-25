#pragma once
#include<filesystem>
#include<stdexcept>
#include<string>
#include<thread>
#include<vector>
#include<stdlib.h>

// fresh folder per test, removed afterwards
struct Test_dir
{
    std::filesystem::path path;

    Test_dir()
    {
        std::string name = (std::filesystem::temp_directory_path() / "coalesce_test_XXXXXX").string();
        if(!mkdtemp(name.data())) throw std::runtime_error("mkdtemp failed");
        path = name;
    }
    ~Test_dir() { std::filesystem::remove_all(path); }
    std::string db() const { return (path / "test.db").string(); }
};

template<typename F>
void run_threads(int n, F f)
{
    std::vector<std::jthread> threads;
    for(int i = 0; i < n; i++) threads.emplace_back([&f, i] { f(i); });
}

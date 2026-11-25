#include<chrono>
#include<csignal>
#include<filesystem>
#include<iostream>
#include<string>
#include<thread>
#include<ctime>
#include"flusher.hpp"
#include"server.hpp"
#include"store.hpp"

struct Options
{
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string db = "likes.db";
    int flush_ms = 1000;
};

static void print_usage()
{
    std::cout << "usage: coalesce [--host 127.0.0.1] [--port 8080] [--db likes.db] [--flush-ms 1000]" << std::endl;
}

static bool read_number(const std::string &text, int min, int max, int &out)
{
    try
    {
        std::size_t used = 0;
        int number = std::stoi(text, &used);
        if(used != text.size() || number < min || number > max) return false;
        out = number;
        return true;
    }
    catch(const std::exception &)
    {
        return false;
    }
}

static bool read_options(int argc, char **argv, Options &options)
{
    for(int i = 1; i < argc; i += 2)
    {
        std::string key = argv[i];
        if(i + 1 >= argc) return false;
        std::string value = argv[i + 1];
        if(key == "--host") options.host = value;
        else if(key == "--db") options.db = value;
        else if(key == "--port" && read_number(value, 1, 65535, options.port)) continue;
        else if(key == "--flush-ms" && read_number(value, 1, 60000, options.flush_ms)) continue;
        else return false;
    }
    return true;
}

// sleeps until ctrl+c or SIGTERM, then stops the server
static void wait_for_signal(std::stop_token stop, const sigset_t &signals, Server &server)
{
    timespec tick = {0, 200000000};
    while(!stop.stop_requested())
    {
        if(sigtimedwait(&signals, nullptr, &tick) > 0)
        {
            std::cout << "\nstopping" << std::endl;
            server.svr.wait_until_ready();
            server.stop();
            return;
        }
    }
}

int main(int argc, char **argv) {
    Options options;
    if(argc == 2 && std::string(argv[1]) == "--help")
    {
        print_usage();
        return 0;
    }
    if(!read_options(argc, argv, options))
    {
        print_usage();
        return 2;
    }

    // block before any thread starts, so only wait_for_signal sees them
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    try
    {
        Store store(options.db);
        Counter counter(store);
        std::jthread flusher(update_count, std::ref(counter), std::chrono::milliseconds(options.flush_ms));

        Server server(counter, store, std::filesystem::absolute(options.db).parent_path());
        if(!server.svr.bind_to_port(options.host, options.port))
        {
            std::cerr << "cannot listen on " << options.host << ":" << options.port << ", is it already in use?" << std::endl;
            return 1;
        }
        std::cout << "coalesce on http://" << options.host << ":" << options.port << " saving to " << options.db << std::endl;

        std::jthread signal_thread(wait_for_signal, std::cref(signals), std::ref(server));
        server.svr.listen_after_bind(); // returns once every request is finished
        signal_thread.request_stop();
        signal_thread.join();

        flusher.request_stop();
        flusher.join(); // does the final flush
        std::cout << "saved " << counter.durable() << std::endl;
        if(counter.durable() != counter.accepted())
        {
            std::cerr << counter.accepted() - counter.durable() << " likes could not be saved" << std::endl;
            return 1;
        }
    }
    catch(const std::exception &e)
    {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

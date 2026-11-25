#include"flusher.hpp"
#include<condition_variable>
#include<iostream>

void flush(const std::vector<Strategy *> &strategies, const std::function<void(std::int64_t)> &write)
{
    std::vector<std::int64_t> now;
    std::int64_t delta = 0;
    for(Strategy *s : strategies)
    {
        now.push_back(s->total());
        delta += now.back() - s->last_written;
    }

    if(delta != 0) write(delta); // may throw, then nothing below runs

    for(std::size_t i = 0; i < strategies.size(); i++)
    {
        strategies[i]->last_written = now[i];
    }
}

Counter::Counter(Store &s) : store(s), start_value(s.read())
{
    for(std::size_t i = 0; i < live_strategies().size(); i++)
    {
        const std::string &name = live_strategies()[i];
        strategies.push_back(make_strategy(name, store));
        if(strategies.back()->batched()) batched.push_back(strategies.back().get());
        if(name == "atomic_batch") active = i;
    }
}

void Counter::add(std::int64_t delta)
{
    strategies[active.load()]->add(delta);
}

std::int64_t Counter::accepted() const
{
    std::int64_t sum = start_value;
    for(const auto &s : strategies) sum += s->total();
    return sum;
}

std::int64_t Counter::durable() const
{
    std::int64_t sum = start_value;
    for(const auto &s : strategies) sum += s->last_written;
    return sum;
}

bool Counter::set_active(const std::string &name)
{
    for(std::size_t i = 0; i < live_strategies().size(); i++)
    {
        if(live_strategies()[i] == name)
        {
            active = i; // old strategy keeps being flushed, nothing is lost
            return true;
        }
    }
    return false;
}

std::string Counter::active_name() const
{
    return live_strategies()[active.load()];
}

bool Counter::try_flush()
{
    try
    {
        //critical section: two flushes must not write the same delta
        std::lock_guard<std::mutex> lock(flush_lock);
        flush(batched, [this](std::int64_t delta) { store.add(delta); });
        return true;
    }
    catch(const SQLite::Exception &e)
    {
        flush_errors++;
        std::cerr << "db write failed: " << e.what() << std::endl;
        return false;
    }
}

void update_count(std::stop_token stop, Counter &counter, std::chrono::milliseconds interval)
{
    std::mutex m;
    std::condition_variable_any cv;
    while(!stop.stop_requested())
    {
        {
            std::unique_lock<std::mutex> lock(m);
            cv.wait_for(lock, stop, interval, [] { return false; });
        }
        counter.try_flush();
    }
    // on stop the wait returns early and the loop body flushes one last time,
    // unless stop arrived during a flush. one more is free when nothing is left
    for(int i = 0; i < 3 && !counter.try_flush(); i++)
    {
    }
}

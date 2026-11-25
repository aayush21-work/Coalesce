#include"server.hpp"
#include<algorithm>
#include<chrono>
#include<iostream>
#include<nlohmann/json.hpp>
#include"web_assets.hpp"

using json = nlohmann::json;
using server_clock = std::chrono::steady_clock;

static constexpr int worker_threads = 64;
static constexpr int max_streams = 16; // each one keeps a worker busy
static constexpr int max_threads = 64;
static constexpr int max_seconds = 10;
static constexpr std::size_t history_points = 240;
static constexpr auto sample_interval = std::chrono::milliseconds(250);

static void send_error(httplib::Response &res, int status, const std::string &message)
{
    res.status = status;
    res.set_content(json{{"error", message}}.dump(), "application/json");
}

// requiring json makes browsers ask before another site can call these
static bool read_json(const httplib::Request &req, httplib::Response &res, json &body)
{
    std::string type = req.get_header_value("Content-Type");
    type = type.substr(0, type.find(';'));
    if(type != "application/json")
    {
        send_error(res, 415, "send Content-Type: application/json");
        return false;
    }
    body = json::parse(req.body, nullptr, false);
    if(!body.is_object())
    {
        send_error(res, 400, "body must be a json object");
        return false;
    }
    return true;
}

// missing is fine and keeps the default, anything else must be an int in range
static bool read_int(const json &body, const std::string &key, int max, int &out)
{
    if(!body.contains(key)) return true;
    const json &value = body[key];
    if(!value.is_number_integer()) return false;
    if(value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(max)) return false;
    std::int64_t number = value.get<std::int64_t>();
    if(number < 1 || number > max) return false;
    out = static_cast<int>(number);
    return true;
}

Server::Server(Counter &c, Store &s, const std::filesystem::path &bench_dir)
    : counter(c), store(s), bench(bench_dir)
{
    svr.new_task_queue = [] { return new httplib::ThreadPool(worker_threads); };
    svr.set_tcp_nodelay(true);
    // httplib turns on SO_REUSEPORT, which lets a second server silently share the port
    svr.set_socket_options([](socket_t sock) {
            int yes = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    });
    svr.set_payload_max_length(4096);
    svr.set_default_headers({
        {"X-Content-Type-Options", "nosniff"},
        {"Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'"},
        {"Cache-Control", "no-store"}});
    svr.set_exception_handler([](const httplib::Request & /*req*/, httplib::Response &res, std::exception_ptr ep) {
            try
            {
                std::rethrow_exception(ep);
            }
            catch(const std::exception &e)
            {
                std::cerr << "request failed: " << e.what() << std::endl;
            }
            catch(...)
            {
            }
            send_error(res, 500, "internal error");
    });
    add_routes();
    sampler = std::jthread([this](std::stop_token stop) { sample(stop); });
}

Server::~Server()
{
    stop();
    sampler.request_stop();
    sampler.join();
    bench.stop();
}

void Server::stop()
{
    {
        std::lock_guard<std::mutex> lock(m);
        stopping = true;
    }
    cv.notify_all();
    svr.stop();
}

void Server::change(std::int64_t delta, httplib::Response &res)
{
    auto before = server_clock::now();
    counter.add(delta);
    auto after = server_clock::now();
    metrics.record(std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
    res.set_content("{\"count\":" + std::to_string(counter.accepted()) + "}", "application/json");
}

void Server::add_routes()
{
    svr.Post("/like", [this](const httplib::Request & /*req*/, httplib::Response &res) {
            change(1, res);
    });

    svr.Post("/dislike", [this](const httplib::Request & /*req*/, httplib::Response &res) {
            change(-1, res);
    });

    svr.Get("/current", [this](const httplib::Request & /*req*/, httplib::Response &res) {
            res.set_content("{\"count\":" + std::to_string(counter.accepted()) + "}", "application/json");
    });

    svr.Put("/api/strategy", [this](const httplib::Request &req, httplib::Response &res) {
            json body;
            if(!read_json(req, res, body)) return;
            if(!body.contains("strategy") || !body["strategy"].is_string() ||
               !counter.set_active(body["strategy"].get<std::string>()))
            {
                send_error(res, 400, "strategy must be write_through, mutex_batch, atomic_batch or sharded_batch");
                return;
            }
            res.set_content(json{{"strategy", counter.active_name()}}.dump(), "application/json");
    });

    svr.Post("/api/bench", [this](const httplib::Request &req, httplib::Response &res) {
            json body;
            if(!read_json(req, res, body)) return;

            Bench_config config;
            if(body.contains("strategy"))
            {
                if(!body["strategy"].is_string())
                {
                    send_error(res, 400, "strategy must be a string");
                    return;
                }
                config.strategy = body["strategy"].get<std::string>();
            }
            const auto &names = bench_strategies();
            if(config.strategy != "all" && std::find(names.begin(), names.end(), config.strategy) == names.end())
            {
                send_error(res, 400, "unknown strategy");
                return;
            }
            if(!read_int(body, "threads", max_threads, config.threads))
            {
                send_error(res, 400, "threads must be 1 to " + std::to_string(max_threads));
                return;
            }
            if(!read_int(body, "seconds", max_seconds, config.seconds))
            {
                send_error(res, 400, "seconds must be 1 to " + std::to_string(max_seconds));
                return;
            }
            if(!bench.start(config))
            {
                send_error(res, 409, "a benchmark is already running");
                return;
            }
            res.status = 202;
            res.set_content(bench.state().dump(), "application/json");
    });

    svr.Get("/api/bench", [this](const httplib::Request & /*req*/, httplib::Response &res) {
            res.set_content(bench.state().dump(), "application/json");
    });

    svr.Get("/api/stream", [this](const httplib::Request &req, httplib::Response &res) {
            stream(req, res);
    });

    svr.Get("/", [](const httplib::Request & /*req*/, httplib::Response &res) {
            res.set_content(web_index_html, "text/html; charset=utf-8");
    });
    svr.Get("/style.css", [](const httplib::Request & /*req*/, httplib::Response &res) {
            res.set_content(web_style_css, "text/css; charset=utf-8");
    });
    svr.Get("/app.js", [](const httplib::Request & /*req*/, httplib::Response &res) {
            res.set_content(web_app_js, "text/javascript; charset=utf-8");
    });
    svr.Get("/favicon.svg", [](const httplib::Request & /*req*/, httplib::Response &res) {
            res.set_content(web_favicon_svg, "image/svg+xml");
    });
}

// server sent events: one history message, then a sample every 250 ms
void Server::stream(const httplib::Request & /*req*/, httplib::Response &res)
{
    if(streams.fetch_add(1) >= max_streams)
    {
        streams--;
        send_error(res, 503, "too many live viewers");
        return;
    }

    auto seen = std::make_shared<std::uint64_t>(0);
    res.set_chunked_content_provider("text/event-stream",
        [this, seen](std::size_t /*offset*/, httplib::DataSink &sink) {
            std::string out;
            {
                std::unique_lock<std::mutex> lock(m);
                cv.wait(lock, [&] { return stopping || sequence > *seen; });
                if(stopping)
                {
                    sink.done();
                    return true;
                }
                if(*seen == 0)
                {
                    out = "event: history\ndata: [";
                    for(std::size_t i = 0; i < history.size(); i++)
                    {
                        if(i > 0) out += ",";
                        out += history[i];
                    }
                    out += "]\n\n";
                }
                out += "data: " + latest + "\n\n";
                *seen = sequence;
            }
            return sink.write(out.data(), out.size());
        },
        [this](bool /*success*/) { streams--; });
}

// turns running counters into rates. rates cover the last second,
// percentiles the last two seconds
void Server::sample(std::stop_token stop)
{
    struct Point
    {
        server_clock::time_point time;
        std::uint64_t requests;
        std::uint64_t writes;
        std::vector<std::uint64_t> latency;
    };
    auto take = [this] { return Point{server_clock::now(), metrics.requests, store.writes, metrics.counts()}; };

    std::deque<Point> points = {take()};
    auto next = server_clock::now();
    while(!stop.stop_requested())
    {
        next += sample_interval;
        std::this_thread::sleep_until(next);

        points.push_back(take());
        if(points.size() > 9) points.pop_front();
        const Point &now = points.back();
        const Point &second_ago = points.size() > 5 ? points[points.size() - 5] : points.front();

        double seconds = std::chrono::duration<double>(now.time - second_ago.time).count();
        double requests_per_second = (now.requests - second_ago.requests) / seconds;
        double writes_per_second = (now.writes - second_ago.writes) / seconds;

        std::vector<std::uint64_t> window = now.latency;
        for(std::size_t i = 0; i < window.size(); i++) window[i] -= points.front().latency[i];
        auto p50 = histogram::percentile(window, 0.50);
        auto p99 = histogram::percentile(window, 0.99);

        // durable first, so a like arriving in between counts as unsaved
        std::int64_t durable = counter.durable();
        std::int64_t accepted = counter.accepted();
        json state = {
            {"count", accepted},
            {"durable", durable},
            {"unsaved", accepted - durable},
            {"strategy", counter.active_name()},
            {"requests", now.requests},
            {"db_writes", now.writes},
            {"requests_per_second", requests_per_second},
            {"writes_per_second", writes_per_second},
            {"p50_ns", p50 ? json(*p50) : json(nullptr)},
            {"p99_ns", p99 ? json(*p99) : json(nullptr)},
            {"flush_errors", counter.flush_errors.load()},
            {"viewers", streams.load()},
            {"bench", bench.state()}};
        std::string point = json{{"requests_per_second", requests_per_second},
                                 {"writes_per_second", writes_per_second}}.dump();
        {
            std::lock_guard<std::mutex> lock(m);
            latest = state.dump();
            history.push_back(point);
            if(history.size() > history_points) history.pop_front();
            sequence++;
        }
        cv.notify_all();
    }
}

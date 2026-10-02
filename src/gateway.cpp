#include "config.hpp"
#include "health.hpp"
#include "net_guard.hpp"
#include "auth.hpp"
#include "wol.hpp"
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
#include <map>
#include <atomic>



namespace {

    // Atomics
    struct BackendState {
        std::atomic<int> in_flight{0};
        std::atomic<bool> power_op{false};
    };

    struct InFlightGuard {
        std::atomic<int>& counter;

        explicit InFlightGuard(std::atomic<int>& c) : counter(c) { ++counter; }
        ~InFlightGuard() { --counter; }

        InFlightGuard(const InFlightGuard&) = delete;
        InFlightGuard& operator=(const InFlightGuard&) = delete;
    };

    struct FlagGuard {
        std::atomic<bool>& flag;
        explicit FlagGuard(std::atomic<bool>& f) : flag(f) {}
        ~FlagGuard() { flag = false; }

        FlagGuard(const FlagGuard&) = delete;
        FlagGuard& operator=(const FlagGuard&) = delete;
    };

    using json = nlohmann::json;

    using ModelTable = std::map<std::string, gw::Backend>;


    void send_error(httplib::Response& res, int status, const std::string& message) {
        res.status = status;
        res.set_content(json{{"error", message}}.dump(), "application/json");
    }


    bool has_bearer_key(const httplib::Request& req, const std::string& key) {
        const std::string header = req.get_header_value("Authorization");
        return header.starts_with("Bearer ") &&
            gw::constant_time_equal(header.substr(7), key);
    }


    ModelTable build_model_table(const std::vector<gw::Backend>& backends) {
        ModelTable table;
        for (const gw::Backend& b : backends) {
            for (const std::string& m : b.models) {
                table.emplace(m, b);
            }
        }
        return table;
    }


    const gw::Backend* find_backend(const std::vector<gw::Backend>& backends, const std::string& name) {
        for (const gw::Backend& b : backends) {
            if (b.name == name) return &b;
        }
        return nullptr;
    }


    void handle_chat(const ModelTable models, const std::string& default_model,
                    const httplib::Request& req, httplib::Response& res,
                    std::map<std::string, BackendState>& states) {
        // Parse json
        json body;
        std::string model;
        try {
            body = json::parse(req.body);
            model = body.value("model", default_model);
        } catch (const json::exception& e) {
            send_error(res, 400, e.what());
            return;
        }

        // model lookup
        auto it = models.find(model);
        if (it == models.end()) {
            send_error(res, 404, "unknown model: " + model);
            return;
        }
        const gw::Backend& backend = it->second;
        std::cerr << "request for " << model << " -> " << backend.name << '\n';    // log

        // Create Guard
        InFlightGuard guard(states.at(backend.name).in_flight); // counter++

        if (states.at(backend.name).power_op.load()) {
            return send_error(res, 503, "backend '" + backend.name + "' is busy with a power operation, try again later");
        }

        httplib::Client client(backend.host, backend.port);           // create client
        client.set_read_timeout(std::chrono::seconds(300));           // set timeout
        client.set_connection_timeout(std::chrono::seconds(2));

        if (!gw::is_healthy(backend.host, backend.port)) {
            send_error(res, 503, "backend '" + backend.name + "' is not running; wake it first");
            return;
        }
        // forward
        auto upstream = client.Post("/v1/chat/completions", req.body, "application/json");
        if (!upstream) {
            if (upstream.error() == httplib::Error::Read) {
                send_error(res, 504, "backend took too long to answer");
            } else {
                send_error(res, 502, "backend did not respond: " + httplib::to_string(upstream.error()));
            }
            return;
        }
        res.status = upstream->status;
        res.set_content(upstream->body, "application/json");
    }



    void handle_wake(const std::vector<gw::Backend>& backends, const httplib::Request& req,
                    httplib::Response& res, std::map<std::string, BackendState>& states) {
        const std::string name = req.matches[1];
        const auto b = find_backend(backends, name);
        // 404 not found
        if (b == nullptr){
            std::cerr << "backend not found at " << req.path << '\n';
            return send_error(res, 404, "backend not found");
        }
        // 400 no wake config
        if (!b->wake) {
            std::cerr << "backend '" << name << "' has no wake config" << '\n';
            return send_error(res, 400, "backend '" + name + "' cannot be woken");
        }
        // 200 already healthy
        if (gw::is_healthy(b->host, b->port)) {
            res.status = 200;
            res.set_content(json{{"status", "already up"}}.dump(), "application/json");
            return;
        }

        if (states.at(b->name).power_op.exchange(true)) {
            return send_error(res, 409, "a power operation is already in progress on '" + name + "'");
        }
        FlagGuard clear_on_exit(states.at(b->name).power_op);
        // Stopwatch, 500 on failure
        const auto start_time = std::chrono::steady_clock::now();
        if (!gw::send_magic_packet(b->wake->mac, b->wake->broadcast)) {
            std::cerr << "error sending magic packet to backend " << name << '\n';
            return send_error(res, 500, "error sending magic packet");
        }
        const bool up = gw::wait_until_healthy(b->host, b->port, std::chrono::seconds(b->wake->timeout_s));
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        const double elapsed_rounded = std::round(elapsed * 10) / 10;
        if (up) {
            res.set_content(json{{"status", "up"}, {"seconds", elapsed_rounded}}.dump(), "application/json");
            return;
        }
        std::cerr << "backend '" << name << "' timed out after " << elapsed_rounded << "seconds\n";
        return send_error(res, 504, "timed out");
    }
}

int main(int argc, char** argv){
    // usage check
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <config.json>\n";
        return 2;
    }
    gw::Config conf;
    try {
        conf = gw::load_config(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "config error: " << e.what() << '\n';
        return 1;
    }
    // States map for backends
    std::map<std::string, BackendState> states;
    for (const gw::Backend& b : conf.backends) {
        states[b.name];
    }

    httplib::Server svr;

    const ModelTable models = build_model_table(conf.backends);

    svr.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
        // Check IP
        std::optional<std::uint32_t> client_ip = gw::parse_ipv4(req.remote_addr);
        if (!client_ip || !conf.allowed_clients.contains(*client_ip)) {
            send_error(res, 403, "forbidden: unknown IP address");
            std::cerr << "rejected client " << req.remote_addr << '\n';
            return httplib::Server::HandlerResponse::Handled;
        }
        // Check keys
        if (req.path.starts_with("/v1/")) {
            if (!has_bearer_key(req, conf.api_key)){
                res.set_header("WWW-Authenticate", "Bearer");
                send_error(res, 401, "missing or invalid API key");
                std::cerr << "unauthorized request from " << req.remote_addr << " to " << req.path << '\n';
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        if (req.path.starts_with("/admin/")) {
            if (!has_bearer_key(req, conf.admin_key)) {
                res.set_header("WWW-Authenticate", "Bearer");
                send_error(res, 401, "missing or invalid admin key");
                std::cerr << "unauthorized request from " << req.remote_addr << " to " << req.path << '\n';
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(json{{"status", "ok"}}.dump(), "application/json");
    });

    svr.Get("/admin/status", [&](const httplib::Request&, httplib::Response& res) {
        json list = json::array();
        for (const gw::Backend& b : conf.backends) {
            list.push_back({
                {"name", b.name},
                {"models", b.models},
                {"up", gw::is_healthy(b.host, b.port)},
                {"can_wake", b.wake.has_value()},
                {"in_flight", states.at(b.name).in_flight.load()},
                {"power_op", states.at(b.name).power_op.load()},
            });
        }
        res.set_content(json{{"backends", list}}.dump(), "application/json");
    });

    svr.Post(R"(/admin/([A-Za-z0-9_-]+)/wake)", [&](const httplib::Request& req, httplib::Response& res) {
        handle_wake(conf.backends, req, res, states);
    });


    svr.Post("/v1/chat/completions", [&](const httplib::Request &req, httplib::Response &res) {
        handle_chat(models, conf.default_model, req, res, states);
    });

    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        std::cerr << req.method << ' ' << req.path << " -> " << res.status << '\n';
    });

    if(!svr.bind_to_port(conf.bind_address, conf.port)) {
        std::cerr << "failed to bind " << conf.bind_address << ":" << conf.port << '\n';
        return 1;
    }

    std::cout << "server listening on " << conf.bind_address << ":" << conf.port << '\n';
    return svr.listen_after_bind() ? 0 : 1;
}

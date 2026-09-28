#include "config.hpp"
#include "health.hpp"
#include "net_guard.hpp"
#include "auth.hpp"
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
#include <map>



namespace {

    using json = nlohmann::json;

    using ModelTable = std::map<std::string, gw::Backend>;

    void send_error(httplib::Response& res, int status, const std::string& message) {
        res.status = status;
        res.set_content(json{{"error", message}}.dump(), "application/json");
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

    void handle_chat(const ModelTable models, const std::string& default_model,
                    const httplib::Request& req, httplib::Response& res) {
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

        httplib::Client client(backend.host, backend.port);           // create client
        client.set_read_timeout(std::chrono::seconds(300)); // set timeout
        client.set_connection_timeout(std::chrono::seconds(2));

        if (!gw::is_healthy(backend.host, backend.port)) {
            send_error(res, 503, "backend '" + backend.name + "' is not running; wake it first");
            return;
        }
        // forward
        auto upstream = client.Post("/v1/chat/completions", req.body, "application/json");
        if (!upstream) {
            send_error(res, 502, "backend did not respond");
            return;
        }
        res.status = upstream->status;
        res.set_content(upstream->body, "application/json");
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
        // Check API key
        if (req.path.starts_with("/v1/")) {
            const std::string header = req.get_header_value("Authorization");
            const bool authorized = header.starts_with("Bearer ") &&
                gw::constant_time_equal(header.substr(7), conf.api_key);
            if (!authorized){
                res.set_header("WWW-Authenticate", "Bearer");
                send_error(res, 401, "missing or invalid API key");
                std::cerr << "unauthorized request from " << req.remote_addr << " to " << req.path << '\n';
                return httplib::Server::HandlerResponse::Handled;
            }
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    svr.Post("/v1/chat/completions", [&](const httplib::Request &req, httplib::Response &res) {
        handle_chat(models, conf.default_model, req, res);
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

#include "health.hpp"
#include <chrono>
#include <httplib.h>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include <map>



namespace {

    struct Backend {
        std::string name;
        std::string host;
        int port;
    };

    using json = nlohmann::json;

    void send_error(httplib::Response& res, int status, const std::string& message) {
        res.status = status;
        res.set_content(json{{"error", message}}.dump(), "application/json");
    }

    void handle_chat(const std::map<std::string, Backend>& models,
                    const httplib::Request& req, httplib::Response& res) {
        // Parse json
        json body;
        std::string model;
        try {
            body = json::parse(req.body);
            model = body.value("model", "qwen3-1.7b");
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
        const Backend& backend = it->second;
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

int main(){
    httplib::Server svr;

    const Backend pi{"pi", "localhost", 8095};
    const Backend pc{"pc", "192.168.1.171", 8095};

    const std::map<std::string, Backend> models = {
        {"qwen3-1.7b",  pi},
        {"gemma-4-12b", pc},
    };

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    svr.Post("/v1/chat/completions", [&](const httplib::Request &req, httplib::Response &res) {
        handle_chat(models, req, res);
    });

    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        std::cerr << req.method << ' ' << req.path << " -> " << res.status << '\n';
    });

    if(!svr.bind_to_port("127.0.0.1", 8100)) {
        std::cerr << "failed to bind 127.0.0.1:8100\n";
        return 1;
    }

    std::cout << "server listening on 127.0.0.1:8100\n";
    return svr.listen_after_bind() ? 0 : 1;
}

#include <chrono>
#include <httplib.h>
#include <iostream>
#include <string>


int main(){
    httplib::Server svr;

    std::string pi_host = "localhost";
    int pi_port = 8095;

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    svr.Post("/v1/chat/completions", [&](const httplib::Request &req, httplib::Response &res) {
        httplib::Client pi(pi_host, pi_port);
        // set timeout
        pi.set_read_timeout(std::chrono::seconds(300));
        auto upstream = pi.Post("/v1/chat/completions", req.body, "application/json");
        if (!upstream) {
            res.status = 502;
            res.set_content(R"({"error":"backend did not respond"})", "application/json");
            return;
        }
        res.status = upstream->status;
        res.set_content(upstream->body, "application/json");
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

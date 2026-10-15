#include "health.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <optional>


namespace gw {
namespace {
    void configure(httplib::Client& client) {
        client.set_connection_timeout(std::chrono::seconds(1));  // time to establish the connection
        client.set_read_timeout(std::chrono::seconds(2));        // time to wait for the answer once connected
    }
}
    bool is_healthy(const std::string& host, int port){
        httplib::Client client(host, port);
        configure(client);
        auto res = client.Get("/health");   // res behaves a lot like a pointer
        return res && res->status == 200;
    }

    bool wait_until_healthy(const std::string& host, int port, std::chrono::seconds timeout){
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline){
            if (is_healthy(host, port)) return true;
            std::cout << '.' << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return false;
    }

    // Asks llama-server for its context size. nullopt if unreachable or field missing.
    std::optional<int> fetch_n_ctx(const std::string& host, int port) {
        //body.at("default_generation_settings").at("n_ctx").get<int>()
        httplib::Client client(host, port);
        configure(client);
        auto res = client.Get("/props");
        if (!res || res->status != 200) return std::nullopt;
        const auto j = nlohmann::json::parse(res->body, nullptr, false);
        if (j.is_discarded()) return std::nullopt;
        if (!j.contains("default_generation_settings")) return std::nullopt;
        const auto& dgs = j.at("default_generation_settings");
        if (!dgs.contains("n_ctx") || !dgs.at("n_ctx").is_number_integer()) return std::nullopt;
        return dgs.at("n_ctx").get<int>();
    }
}

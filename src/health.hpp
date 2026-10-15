#pragma once
#include <string>
#include <chrono>

namespace gw{
    bool is_healthy(const std::string& host, int port);
    bool wait_until_healthy(const std::string& host, int port, std::chrono::seconds timeout);
    // Asks llama-server for its context size. nullopt if unreachable or field missing.
    std::optional<int> fetch_n_ctx(const std::string& host, int port);
}

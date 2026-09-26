#pragma once
#include <string>
#include <chrono>

namespace gw{
    bool is_healthy(const std::string& host, int port);
    bool wait_until_healthy(const std::string& host, int port, std::chrono::seconds timeout);
}

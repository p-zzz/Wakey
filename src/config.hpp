#pragma once
#include "net_guard.hpp"
#include "wol.hpp"
#include <optional>
#include <string>
#include <vector>

namespace gw {

struct WakeConfig {
    gw::MacAddress mac;
    std::string broadcast;
};

struct Backend {
    std::string name;
    std::string host;
    int port;
    std::vector<std::string> models;
    std::optional<WakeConfig> wake;
};

struct Config {
    std::string api_key;
    std::string admin_key;
    std::string bind_address;
    int port;
    int timeout_s;
    Ipv4Cidr allowed_clients;
    std::string default_model;
    std::vector<Backend> backends;
};

Config load_config(const std::string& path);

}

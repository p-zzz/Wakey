#pragma once
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
    std::string bind_address;
    int port;
    std::string allowed_clients;
    std::string default_model;
    std::vector<Backend> backends;
};

Config load_config(const std::string& path);

}

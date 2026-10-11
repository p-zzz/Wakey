#pragma once
#include <string>

namespace gw {
    struct ClientConfig {
        std::string url;
        std::string api_key;
        std::string admin_key;
        std::string default_model;
    };

    ClientConfig load_client_config(const std::string& path);
}

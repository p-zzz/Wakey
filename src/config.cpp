#include "config.hpp"
#include <nlohmann/json.hpp>
#include "wol.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <set>

namespace gw {

namespace {

using json = nlohmann::json;

Backend parse_backend(const json& jb) {
    Backend b;
    b.name = jb.at("name").get<std::string>();
    b.host = jb.at("host").get<std::string>();
    b.port = jb.at("port").get<int>();
    if (b.port < 1 || b.port > 65535) {
        throw std::runtime_error("backend '" + b.name + "': port: " + std::to_string(b.port) + " out of range (1-65535)");
    }

    b.models = jb.at("models").get<std::vector<std::string>>();
    if (b.models.empty()) {
        throw std::runtime_error("backend '" + b.name + "': no valid models found");
    }

    if (jb.contains("wake")) {
        const json& jw = jb.at("wake");
        WakeConfig w;
        const std::string mac_text = jw.at("mac").get<std::string>();
        if (!parse_mac_addr(mac_text, w.mac)) {
            throw std::runtime_error("backend '" + b.name + "': invalid wake.mac: " + mac_text);
        }
        // broadcast
        w.broadcast = jw.at("broadcast").get<std::string>();

        b.wake = w;
    }
    return b;
}

}   // namespace

Config load_config(const std::string &path){
    std::ifstream in(path);
    if (!in) {
        //throw error
        throw std::runtime_error("cannot open config file " + path);
    }
    const json j = json::parse(in);

    Config conf;
    conf.bind_address = j.at("bind_address").get<std::string>();
    conf.port = j.at("port").get<int>();
    if (conf.port < 1 || conf.port > 65535) {
        throw std::runtime_error("port out of range (1-65535)");
    }

    conf.allowed_clients = j.at("allowed_clients").get<std::string>();
    conf.default_model = j.at("default_model").get<std::string>();

    for (const json& jb : j.at("backends")) {
        conf.backends.push_back(parse_backend(jb));
    }
    if (conf.backends.empty()) {
        throw std::runtime_error("cannot find backends");
    }

    std::set<std::string> seen;
    for (const Backend& b : conf.backends) {
        for (const std::string& m : b.models) {
            if (!seen.insert(m).second) {
                throw std::runtime_error("model '" + m + "' is listed on more than one backend");
            }
        }
    }
    if (!seen.contains(conf.default_model)) {
        throw std::runtime_error("default model '" + conf.default_model + "' is not listed in any backend");
    }

    return conf;
    }
}

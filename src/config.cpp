#include "config.hpp"
#include "net_guard.hpp"
#include <nlohmann/json.hpp>
#include "wol.hpp"
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <set>
#include <sys/stat.h>

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
        std::optional<std::uint32_t> broadcast = parse_ipv4(w.broadcast);
        if (!broadcast) {
            throw std::runtime_error("backend'" + b.name + "': invalid wake.broadcast: " + w.broadcast);
        }

        b.wake = w;
    }
    return b;
}

Ipv4Cidr check_allowed_clients(const std::string& clients_text) {
    std::optional<Ipv4Cidr> clients = parse_cidr(clients_text);
    if (!clients) {
        throw std::runtime_error("allowed_clients '" + clients_text + "' is not a valid CIDR range");
    }
    if (clients->prefix == 0) {
        throw std::runtime_error("allowed_clients '0.0.0.0/0' allows every address on the internet; use WireGuard or your local IP");
    }
    if (!is_private(*clients)) {
        throw std::runtime_error("allowed_clients '" + clients_text + "' includes addresses outside the private ranges (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 127.0.0.0/8)");
    }
    return *clients;
}

void check_bind_address(const std::string& text, const Ipv4Cidr& clients) {
    std::optional<std::uint32_t> bind_address = parse_ipv4(text);
    if (!bind_address) {
        throw std::runtime_error("bind address '" + text + "' is not a valid IP address");
    }
    if (*bind_address == 0) {
        throw std::runtime_error("bind_address '0.0.0.0' listens on every interface, including public ones; use your WireGuard IP or '127.0.0.1'");
    }
    if (!clients.contains(*bind_address)) {
        throw std::runtime_error("bind_address '" + text + "' is not inside allowed_clients; bind to the address on that network");
    }
}

std::string read_key_file(const std::string& path) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        throw std::runtime_error(path + " : file doesn't exist or can't be accessed");
    }
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        throw std::runtime_error("access refused; to make your key private run: chmod 600 <path>");
    }

    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("error opening file " + path);
    }
    std::string key;
    std::getline(in, key);
    if (key.size() < 32) {
        throw std::runtime_error(path + " file is empty or invalid key");
    }

    return key;
}

}   // namespace

Config load_config(const std::string &path){
    std::ifstream in(path);
    if (!in) {
        //throw error
        throw std::runtime_error("cannot open config file " + path);
    }
    const json j = json::parse(in);

    // Check keys

    Config conf;

    conf.api_key = read_key_file(j.at("api_key_file").get<std::string>());
    conf.admin_key = read_key_file(j.at("admin_key_file").get<std::string>());

    conf.bind_address = j.at("bind_address").get<std::string>();

    conf.port = j.at("port").get<int>();
    if (conf.port < 1 || conf.port > 65535) {
        throw std::runtime_error("port out of range (1-65535)");
    }

    std::string allowed_clients = j.at("allowed_clients").get<std::string>();
    conf.allowed_clients = check_allowed_clients(allowed_clients);
    check_bind_address(conf.bind_address, conf.allowed_clients);

    conf.default_model = j.at("default_model").get<std::string>();

    conf.timeout_s = j.at("timeout_s").get<int>();
    if (conf.timeout_s < 1 || conf.timeout_s > 420) {
        throw::std::runtime_error("timeout negative or too long");
    }

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

}   // gw namespace

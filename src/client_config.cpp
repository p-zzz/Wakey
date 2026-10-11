#include "client_config.hpp"
#include "keyfile.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace gw {

using json = nlohmann::json;

ClientConfig load_client_config(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open config file " + path);
    }
    const json j = json::parse(in);

    ClientConfig cli;

    std::string api_path = expand_home(j.at("api_key_file").get<std::string>());
    std::string admin_path = expand_home(j.at("admin_key_file").get<std::string>());

    cli.api_key = read_key_file(api_path);
    cli.admin_key = read_key_file(admin_path);

    cli.default_model = j.at("default_model").get<std::string>();

    cli.url = j.at("url").get<std::string>();
    if (!cli.url.starts_with("http://")) throw std::runtime_error("url needs to start with http://");

    return cli;
}

}   // namespace gw

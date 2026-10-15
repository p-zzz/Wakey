#include "client_config.hpp"
#include "keyfile.hpp"

#include <iostream>
#include <nlohmann/json.hpp>
#include <httplib.h>
#include <iomanip>
#include <stdexcept>
#include <string>



using json = nlohmann::json;

void print_usage() {
    std::cerr << "usage: wakeyctl status\n"
              << std::setw(7)
              << "wakeyctl wake <backend>\n"
              << std::setw(7)
              << "wakeyctl stop <backend>\n"
              << std::setw(7)
              << "wakeyctl poweroff [-y|--yes] <backend>"
              << std::setw(7)
              << "wakeyctl ask [--think] \"question\" [model]\n";
}

bool confirm(const std::string& question) {
    std::cout << question;
    std::string ans;
    std::getline(std::cin, ans);
    if (ans == "Y" || ans == "y" || ans == "Yes" || ans == "yes") return true;
    return false;
}

httplib::Headers get_header(const std::string key) {
    return httplib::Headers{{"Authorization", "Bearer " + key}};
}

httplib::Client make_client(const gw::ClientConfig& conf) {
    httplib::Client cli(conf.url);
    cli.set_connection_timeout(std::chrono::seconds(5));
    cli.set_read_timeout(std::chrono::seconds(600));
    return cli;
}

json check_response(httplib::Result& res, const gw::ClientConfig& conf) {
    if (!res) throw std::runtime_error("cannot reach Wakey at " + conf.url);
    if (res->status < 200 || res->status >= 300) {
        std::string msg = "HTTP " + std::to_string(res->status);
        if (json::accept(res->body)) {
            const json err = json::parse(res->body);
            const json& e = err.at("error");
            if (e.is_string()) msg += ": " + e.get<std::string>();
            else if (e.is_object() && e.contains("message")) msg += ": " + e.at("message").get<std::string>();
        }
        throw std::runtime_error(msg);
    }
    return json::parse(res->body);
}

// prints the reply and stats; returns the reply text
std::string print_reply(const json& body, const std::string& model) {
    const json& choice = body.at("choices").at(0);
    const std::string reply = choice.at("message").at("content").get<std::string>();
    std::cout << reply << '\n';
    if (choice.at("finish_reason").get<std::string>() == "length") std::cerr << "(answer truncated)\n";
    const int tokens = body.at("usage").at("completion_tokens").get<int>();
    std::cerr << "[" << model << " · " << tokens << " tokens";
    if (body.contains("timings")) {
        const double tps = body.at("timings").at("predicted_per_second").get<double>();
        std::cerr << " · " << std::fixed << std::setprecision(0) << tps << " t/s";
    }
    std::cerr << "]\n";
    return reply;
}

// sends the conversation, returns the parsed response body
json send_chat(httplib::Client& cli, const gw::ClientConfig& conf,
               const std::string& model, const json& messages, bool think) {
    const json req = {
        {"model", model},
        {"messages", messages},
        {"chat_template_kwargs", {{"enable_thinking", think}}}
    };
    auto res = cli.Post("/v1/chat/completions", get_header(conf.api_key), req.dump(), "application/json");
    const json body = check_response(res, conf);
    return body;
}

json fetch_status(const gw::ClientConfig& conf) {
    auto cli = make_client(conf);
    auto res = cli.Get("/admin/status", httplib::Headers{{"Authorization", "Bearer " + conf.admin_key}});
    const json body = check_response(res, conf);
    return body;
}

int cmd_status(const gw::ClientConfig& conf) {
    const json body = fetch_status(conf);

    for (const auto& b : body.at("backends")) {
        const auto name = b.at("name").get<std::string>();
        const bool up   = b.at("up").get<bool>();
        const int in_fl = b.at("in_flight").get<int>();
        const int power_op = b.at("power_op").get<int>();
        const auto model  = b.at("models").at(0).get<std::string>();
        const bool can_wake = b.at("can_wake").get<bool>();
        const bool can_control = b.at("can_control").get<bool>();

        const std::string state = up ? "up" : "down";

        std::string notes;
        if (power_op)       notes = "power operation in progress";
        else if (up)        notes = std::to_string(in_fl) + " request(s) running";
        else if (can_wake)  notes = "can wake";

        std::cout << std::left
                  << std::setw(6)  << name
                  << std::setw(6)  << state
                  << std::setw(16) << model
                  << notes << '\n';
    }
    return 0;
}


int cmd_wake(const gw::ClientConfig& conf, const std::string& name) {
    auto cli = make_client(conf);
    std::cout << "waking up PC... (this usually takes ~35 s)\n";
    auto res = cli.Post("/admin/" + name + "/wake", get_header(conf.admin_key));
    const json body = check_response(res, conf);
    std::cout << "pc is up after "
              << body.at("total_s").get<std::string>()
              << " s (boot "
              << body.at("boot_s").get<std::string>()
              << " s, load "
              << body.at("load_s").get<std::string>()
              << " s)\n";
    return 0;
}


int cmd_stop(const gw::ClientConfig& conf, const std::string& name) {
    auto cli = make_client(conf);
    auto res = cli.Post("/admin/" + name + "/stop", get_header(conf.admin_key));
    const json body = check_response(res, conf);
    std::cout << "backend "
              << name
              << " stopped\n";
    return 0;
}


int cmd_poweroff(const gw::ClientConfig& conf, const std::string& name, const bool yes) {
    if (!yes && !confirm("Are you sure you want to poweroff pc? [y/N]: ")) {
        std::cout << "aborted\n";
        return 1;
    }
    auto cli = make_client(conf);
    auto res = cli.Post("/admin/" + name + "/poweroff", get_header(conf.admin_key));
    const json body = check_response(res, conf);
    std::cout << name << " shutting down\n";
    return 0;
}


int cmd_ask(const gw::ClientConfig& conf, const std::string& model, const bool think, const std::string& question) {
    auto cli = make_client(conf);
    const json messages = json::array({ {{"role", "user"}, {"content", question}} });
    const json body = send_chat(cli, conf, model, messages, think);
    print_reply(body, model);
    return 0;
}


int cmd_chat(const gw::ClientConfig& conf, std::string& model, bool think) {
    auto cli = make_client(conf);

    json messages = json::array();
    std::string line;
    while (std::cout << "[" << model << "]> " << std::flush, std::getline(std::cin, line)) {
        try {
            if (line.empty()) continue;
            if (line.starts_with('/')) {
                const auto space = line.find(' ');
                const std::string cmd = line.substr(0, space);
                const std::string arg = (space == std::string::npos) ? "" : line.substr(space + 1);

                if (cmd == "/exit") break;
                if (cmd == "/clear") { messages.clear(); continue; }
                if (line == "/status") { cmd_status(conf); continue; }
                if (line == "/think") { think = !think;  std::cout << "thinking " << (think ? "on" : "off") << '\n'; continue; }
                if (cmd == "/model") {
                    if (arg.empty()) { std::cout << "current model: " << model << '\n'; continue; }
                    const json body = fetch_status(conf);
                    std::vector<std::string> models;
                    bool found = false;
                    bool backend_up;
                    std::string backend_name;
                    for (const auto& b : body.at("backends")) {
                        for (const auto& m : b.at("models")) {
                            models.push_back(m.get<std::string>());
                            if (m == arg) {
                                found = true;
                                backend_up = b.at("up").get<bool>();
                                backend_name = b.at("name").get<std::string>();
                            }
                        }
                    }
                    if (!found) {
                        std::cout << "unknown model; available:\n";
                        for (const auto& m : models) std::cout << m << '\n';
                        continue;
                    }
                    model = arg;
                    if (!backend_up) {
                        std::cout << "Model switched, but backend is down, use /wake " << backend_name << "\n";
                    }
                    std::cout << "Now using " << model << "\n";
                    continue;
                }
                if (cmd == "/wake")  {
                    if (!arg.empty()) cmd_wake(conf, arg);
                    else std::cerr << "empty argument, try /help\n";
                    continue;
                }
                if (cmd == "/stop") {
                    if (!arg.empty()) cmd_stop(conf, arg);
                    else std::cerr << "empty argument, try /help\n";
                    continue;
                }
                if (cmd == "/poweroff") {
                    if (!arg.empty()) cmd_poweroff(conf, arg, false);
                    else std::cerr << "empty argument, try /help\n";
                    continue;
                }
                // ...
                std::cout << "unknown command: " << cmd << " (try /help)\n";
                continue;
            }

            messages.push_back({{"role", "user"}, {"content", line}});
            //std::cerr << messages.dump(2) << '\n';   // debug print
            const json body = send_chat(cli, conf, model, messages, think);
            const std::string reply = print_reply(body, model);
            messages.push_back({{"role", "assistant"}, {"content", reply}});
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << '\n';
            if (!messages.empty() && messages.back().at("role") == "user") messages.erase(messages.size() - 1);
        }
    }
    std::cout << '\n';
    return 0;
}


int main(int argc, char** argv) {
    if (argc < 2) { print_usage(); return 2; }
    auto arg = [&](int i) -> std::string { return i < argc ? argv[i] : ""; };
    try {
        const std::string cmd = arg(1);

        const auto conf = gw::load_client_config(gw::expand_home("~/.config/wakey/client.json"));

        if (cmd == "status") return cmd_status(conf);

        if (cmd == "wake" || cmd == "stop") {
            const std::string name = arg(2);
            if (name.empty()) { print_usage(); return 2; }
            return cmd == "wake" ? cmd_wake(conf, name) : cmd_stop(conf, name);
        }

        if (cmd == "poweroff") {
            int i = 2;
            const bool yes = (arg(i) == "-y" || arg(i) == "--yes");
            if (yes) ++i;                           // flag consumed -> name moves one position along
            const std::string name = arg(i);
            if (name.empty()) { print_usage(); return 2; }
            return cmd_poweroff(conf, name, yes);
        }

        if (cmd == "ask") {
            int i = 2;
            const bool think = (arg(i) == "--think");
            if (think) ++i;
            const std::string question = arg(i);
            if (question.empty()) { print_usage(); return 2; }
            const std::string model = arg(i + 1).empty() ? conf.default_model : arg(i + 1);
            return cmd_ask(conf, model, think, question);
        }

        if (cmd == "chat") {
            bool think = (arg(2) == "--think");
            std::string model = arg(3).empty() ? conf.default_model : arg(3);
            return cmd_chat(conf, model, think);
        }

        print_usage();
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}

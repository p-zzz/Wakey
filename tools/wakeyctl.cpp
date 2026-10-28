// wakeyctl: command-line client for Wakey.
//
//   wakeyctl status | wake | stop | poweroff | ask | chat
//
// `chat` is an inline TUI built on replxx: line editing, history, tab completion,
// a spinner while the model works (Ctrl+C cancels), and a status bar with the
// current model, its backend and how full the context is.

#include "client_config.hpp"
#include "keyfile.hpp"
#include "agent_process.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <ostream>
#include <replxx.hxx>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

// Terminal styling

// Colors only when writing to a terminal, and never if NO_COLOR is set
// (https://no-color.org). Decided once in main().
bool g_color_out = false;   // stdout
bool g_color_err = false;   // stderr

namespace ansi {
constexpr const char* reset  = "\033[0m";
constexpr const char* bold   = "\033[1m";
constexpr const char* dim    = "\033[2m";
constexpr const char* red    = "\033[31m";
constexpr const char* green  = "\033[32m";
constexpr const char* yellow = "\033[33m";
constexpr const char* blue   = "\033[34m";
constexpr const char* cyan   = "\033[36m";
}  // namespace ansi

std::string paint(bool on, const char* code, const std::string& text) {
    return on ? std::string(code) + text + ansi::reset : text;
}
std::string out(const char* code, const std::string& text) { return paint(g_color_out, code, text); }
std::string err(const char* code, const std::string& text) { return paint(g_color_err, code, text); }

// Pads to a fixed width BEFORE coloring, so escape codes don't break std::setw alignment.
std::string pad(const std::string& s, std::size_t width) {
    return s.size() >= width ? s + ' ' : s + std::string(width - s.size(), ' ');
}

// 1234 -> "1.2k", 32768 -> "33k", 512 -> "512"
std::string short_count(int n) {
    if (n < 1000) return std::to_string(n);
    std::ostringstream s;
    if (n < 10000) s << std::fixed << std::setprecision(1) << n / 1000.0 << 'k';
    else           s << (n + 500) / 1000 << 'k';
    return s.str();
}

void print_error(const std::string& msg) {
    std::cerr << err(ansi::red, "error: " + msg) << '\n';
}

// Ctrl+C handling and the spinner

// While a request is running the terminal is in normal mode, so Ctrl+C sends SIGINT.
// The handler only sets a flag;
// the spinner loop sees it and cancels the request.
volatile std::sig_atomic_t g_interrupted = 0;
extern "C" void on_sigint(int) { g_interrupted = 1; }

struct SigintGuard {
    struct sigaction old {};
    SigintGuard() {
        g_interrupted = 0;
        struct sigaction sa {};
        sa.sa_handler = on_sigint;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, &old);
    }
    ~SigintGuard() { sigaction(SIGINT, &old, nullptr); }

    SigintGuard(const SigintGuard&) = delete;
    SigintGuard& operator=(const SigintGuard&) = delete;
};

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("cancelled") {}
};

// Shared between the spinner (main thread) and stream_chat (worker thread).
// The spinner draws on the current line until the first piece of text is printed;
// the mutex makes "check + draw" and "switch to text + clear line" impossible to interleave.
struct OutputGate {
    std::mutex m;
    bool streaming = false;

    // stream_chat calls this right before printing a piece of text.
    // The first call clears the spinner line; later calls are a cheap no-op.
    void begin_output() {
        std::lock_guard<std::mutex> lock(m);
        if (streaming) return;
        streaming = true;
        if (isatty(STDERR_FILENO)) std::cerr << "\r\033[K" << std::flush;
    }
};

// Runs `work` (which uses `cli`) on a worker thread while the main thread draws a
// spinner on stderr. Ctrl+C calls cli.stop(), which aborts the HTTP request.
// With a `gate`, the spinner steps aside as soon as streamed text starts printing.
// Returns whatever `work` returns, rethrows its exceptions, throws Cancelled on Ctrl+C.
template <typename F>
auto with_spinner(const std::string& label, httplib::Client& cli, F work, OutputGate* gate = nullptr)
    -> decltype(work()) {
    g_interrupted = 0;
    struct sigaction sa {};
    struct sigaction old {};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old);

    auto result = std::async(std::launch::async, std::move(work));

    static constexpr const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    const bool animate = isatty(STDERR_FILENO);
    const auto start = Clock::now();
    bool stopped = false;

    // true once streamed text owns the line (always false without a gate)
    auto text_started = [&] {
        if (gate == nullptr) return false;
        std::lock_guard<std::mutex> lock(gate->m);
        return gate->streaming;
    };

    for (int i = 0; result.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready; ++i) {
        if (g_interrupted && !stopped) {
            cli.stop();
            stopped = true;
        }
        if (!animate) continue;

        std::unique_lock<std::mutex> lock;
        if (gate != nullptr) lock = std::unique_lock<std::mutex>(gate->m);   // hold while checking + drawing
        if (gate != nullptr && gate->streaming) continue;                    // text has the line now

        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start).count();
        std::cerr << '\r' << err(ansi::cyan, frames[i % 10]) << ' ' << label << "… "
                  << err(ansi::dim, std::to_string(secs) + "s" + (stopped ? "  cancelling" : "  (Ctrl+C to cancel)"))
                  << "\033[K" << std::flush;
    }
    // Erase the spinner line, unless streamed text is on it now
    if (animate && !text_started()) std::cerr << "\r\033[K" << std::flush;
    sigaction(SIGINT, &old, nullptr);

    if (stopped) {
        try { result.get(); } catch (...) {}
        throw Cancelled();
    }
    return result.get();
}

// HTTP helpers

httplib::Headers auth(const std::string& key) {
    return httplib::Headers{{"Authorization", "Bearer " + key}};
}

httplib::Client make_client(const gw::ClientConfig& conf) {
    httplib::Client cli(conf.url);
    cli.set_connection_timeout(std::chrono::seconds(5));
    cli.set_read_timeout(std::chrono::seconds(600));
    return cli;
}

std::string error_message(int status, const std::string& body) {
    std::string msg = "HTTP " + std::to_string(status);
    if (json::accept(body)) {
        const json j = json::parse(body);
        if (j.contains("error")) {
            const json& e = j.at("error");
            if (e.is_string()) msg += ": " + e.get<std::string>();
            else if (e.is_object() && e.contains("message")) msg += ": " + e.at("message").get<std::string>();
        }
    }
    return msg;
}

// Throws on connection errors and non-2xx answers.
// Understands both Wakey's {"error": "..."} and llama-server's {"error": {"message": "..."}}.
json check_response(httplib::Result& res, const gw::ClientConfig& conf) {
    if (!res) throw std::runtime_error("cannot reach Wakey at " + conf.url + " (is WireGuard up? is Wakey running?)");
    if (res->status < 200 || res->status >= 300) {
        std::string msg = error_message(res->status, res->body);
        throw std::runtime_error(msg);
    }
    return json::parse(res->body);
}

json get_status(httplib::Client& cli, const gw::ClientConfig& conf) {
    auto res = cli.Get("/admin/status", auth(conf.admin_key));
    return check_response(res, conf);
}

json fetch_status(const gw::ClientConfig& conf) {
    auto cli = make_client(conf);
    return get_status(cli, conf);
}

json post_admin(httplib::Client& cli, const gw::ClientConfig& conf, const std::string& path) {
    auto res = cli.Post(path, auth(conf.admin_key));
    return check_response(res, conf);
}

// Sends the whole conversation, returns the parsed response body.
json send_chat(httplib::Client& cli, const gw::ClientConfig& conf,
               const std::string& model, const json& messages, bool think) {
    const json req = {
        {"model", model},
        {"messages", messages},
        {"chat_template_kwargs", {{"enable_thinking", think}}}
    };
    auto res = cli.Post("/v1/chat/completions", auth(conf.api_key), req.dump(), "application/json");
    return check_response(res, conf);
}

struct StreamResult {
    std:: string reply;
    int prompt_tokens = 0;
    int completion_tokens = 0;
    double tps = 0;     // from timings, if present
};

StreamResult stream_chat(httplib::Client& cli, const gw::ClientConfig& conf,
                        const std::string& model, const json& messages, bool think, OutputGate& gate) {
    // Append new bytes to a buffer
    StreamResult result;
    std::string buffer;

    const json req = {
        {"model", model},
        {"messages", messages},
        {"stream", true},
        {"stream_options", {{"include_usage", true}}},
        {"chat_template_kwargs", {{"enable_thinking", think}}}
    };

    bool in_reasoning = false;
    auto res = cli.Post("/v1/chat/completions", auth(conf.api_key), req.dump(), "application/json",
            [&](const char* data, size_t len) {
        buffer.append(data, len);
        std::size_t pos;
        while ((pos = buffer.find("\n\n")) != std::string::npos) {
            std::string event = buffer.substr(0, pos);
            buffer.erase(0, pos + 2);
            if (event.starts_with("data: ")) {
                event.erase(0, 6);
                if (event == "[DONE]") break;
                const json& j = json::parse(event, nullptr, false);
                if (j.is_discarded()) continue;
                if (j.contains("choices") && !j.at("choices").empty()) {
                    const json& delta = j.at("choices").at(0).at("delta");
                    if (delta.contains("content") && delta.at("content").is_string()) {
                        if (in_reasoning) { std::cout << '\n'; in_reasoning = false; }
                        const std::string piece = delta.at("content").get<std::string>();
                        gate.begin_output();
                        std::cout << piece << std::flush;
                        result.reply.append(piece);
                    }
                    if (delta.contains("reasoning_content") && delta.at("reasoning_content").is_string()) {
                        in_reasoning = true;
                        gate.begin_output();
                        std::cout  << out(ansi::dim, delta.at("reasoning_content").get<std::string>()) << std::flush;
                    }
                }
                if (j.contains("usage") && j.at("usage").is_object()) {
                    result.prompt_tokens = j.at("usage").at("prompt_tokens").get<int>();
                    result.completion_tokens = j.at("usage").at("completion_tokens").get<int>();
                }
                if (j.contains("timings") && j.at("timings").is_object()) {
                    result.tps = j.at("timings").at("predicted_per_second").get<double>();
                }
            }
        }
        return true;
    });
    if (!res) throw std::runtime_error("cannot reach Wakey at " + conf.url + " (is WireGuard up? is Wakey running?)");
    if (res->status != 200) throw std::runtime_error(error_message(res->status, buffer));
    if (result.reply.empty() && !buffer.empty()) throw std::runtime_error(error_message(res->status, buffer));
    return result;
}

// Reading /admin/status

struct ModelInfo {
    std::string backend;   // name of the backend serving the model
    bool up = false;
    int n_ctx = 0;         // 0 = unknown (backend down, or not reported)
};

// Finds the backend that serves `model`. nullopt if no backend lists it.
std::optional<ModelInfo> find_model(const json& status, const std::string& model) {
    for (const auto& b : status.at("backends")) {
        for (const auto& m : b.at("models")) {
            if (m.get<std::string>() == model) {
                ModelInfo info;
                info.backend = b.at("name").get<std::string>();
                info.up = b.at("up").get<bool>();
                info.n_ctx = b.value("n_ctx", 0);
                return info;
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> all_models(const json& status) {
    std::vector<std::string> names;
    for (const auto& b : status.at("backends"))
        for (const auto& m : b.at("models")) names.push_back(m.get<std::string>());
    return names;
}

std::vector<std::string> all_backends(const json& status) {
    std::vector<std::string> names;
    for (const auto& b : status.at("backends")) names.push_back(b.at("name").get<std::string>());
    return names;
}

void print_status(const json& status) {
    for (const auto& b : status.at("backends")) {
        const auto name     = b.at("name").get<std::string>();
        const bool up       = b.at("up").get<bool>();
        const int in_flight = b.at("in_flight").get<int>();
        const bool power_op = b.at("power_op").get<bool>();
        const auto model    = b.at("models").at(0).get<std::string>();
        const bool can_wake = b.at("can_wake").get<bool>();
        const int n_ctx     = b.value("n_ctx", 0);

        std::string notes;
        if (power_op)      notes = "power operation in progress";
        else if (up)       notes = std::to_string(in_flight) + " request(s) running";
        else if (can_wake) notes = "can wake";
        if (up && n_ctx > 0) notes += " · ctx " + short_count(n_ctx);

        std::cout << out(ansi::bold, pad(name, 6))
                  << (up ? out(ansi::green, pad("up", 6)) : out(ansi::dim, pad("down", 6)))
                  << pad(model, 16)
                  << out(ansi::dim, notes) << '\n';
    }
}

// Prints Wakey's answer to POST /admin/<name>/wake.
void print_wake_result(const std::string& name, const json& body) {
    if (body.at("status").get<std::string>() == "already up") {
        std::cout << name << " is already up\n";
        return;
    }
    const double total = body.at("total_s").get<double>();
    const double boot  = body.at("boot_s").get<double>();
    const double load  = body.at("load_s").get<double>();
    std::ostringstream s;
    s << std::fixed << std::setprecision(1)
      << name << " is up after " << total << " s (boot " << boot << " s, load " << load << " s)";
    std::cout << out(ansi::green, s.str()) << '\n';
}

// Prints the reply and returns its text (for the conversation history).
// `ctx_used` receives prompt + completion tokens: how full the context was.
std::string print_reply(const json& body, int& ctx_used, double seconds) {
    const json& choice = body.at("choices").at(0);
    const std::string reply = choice.at("message").at("content").get<std::string>();
    std::cout << reply << '\n';

    if (choice.at("finish_reason").get<std::string>() == "length")
        std::cerr << err(ansi::yellow, "(answer truncated: hit the token limit)") << '\n';

    const json& usage = body.at("usage");
    const int completion = usage.at("completion_tokens").get<int>();
    ctx_used = usage.at("prompt_tokens").get<int>() + completion;

    std::ostringstream s;
    s << completion << " tokens";
    if (body.contains("timings") && body.at("timings").contains("predicted_per_second"))
        s << " · " << std::fixed << std::setprecision(0) << body.at("timings").at("predicted_per_second").get<double>() << " t/s";
    s << " · " << std::fixed << std::setprecision(1) << seconds << " s";
    std::cerr << err(ansi::dim, s.str()) << '\n';
    return reply;
}

bool confirm(const std::string& question) {
    std::cout << question << std::flush;
    std::string ans;
    if (!std::getline(std::cin, ans)) {
        std::cin.clear();          // interrupted or end of input
        std::cout << '\n';
        return false;
    }
    return ans == "y" || ans == "Y" || ans == "yes" || ans == "Yes";
}

// One-shot commands

void print_usage() {
    std::cerr << "usage: wakeyctl status\n"
                 "       wakeyctl wake <backend>\n"
                 "       wakeyctl stop <backend>\n"
                 "       wakeyctl poweroff [-y|--yes] <backend>\n"
                 "       wakeyctl ask [--think] \"<question>\" [model]\n"
                 "       wakeyctl chat [--think] [model]\n";
}

int cmd_status(const gw::ClientConfig& conf) {
    print_status(fetch_status(conf));
    return 0;
}

int cmd_wake(const gw::ClientConfig& conf, const std::string& name) {
    auto cli = make_client(conf);
    const json body = with_spinner("waking " + name, cli, [&] { return post_admin(cli, conf, "/admin/" + name + "/wake"); });
    print_wake_result(name, body);
    return 0;
}

int cmd_stop(const gw::ClientConfig& conf, const std::string& name) {
    auto cli = make_client(conf);
    post_admin(cli, conf, "/admin/" + name + "/stop");
    std::cout << name << ": model server stopped\n";
    return 0;
}

int cmd_poweroff(const gw::ClientConfig& conf, const std::string& name, bool yes) {
    if (!yes && !confirm("Power off " + name + "? [y/N]: ")) {
        std::cout << "aborted\n";
        return 1;
    }
    auto cli = make_client(conf);
    post_admin(cli, conf, "/admin/" + name + "/poweroff");
    std::cout << name << " is shutting down\n";
    return 0;
}

int cmd_ask(const gw::ClientConfig& conf, const std::string& model, bool think, const std::string& question) {
    auto cli = make_client(conf);
    const json messages = json::array({{{"role", "user"}, {"content", question}}});
    const auto start = Clock::now();
    const json body = with_spinner("thinking", cli, [&] { return send_chat(cli, conf, model, messages, think); });
    int ctx_used = 0;
    print_reply(body, ctx_used, std::chrono::duration<double>(Clock::now() - start).count());
    return 0;
}

// chat: the interactive TUI

struct CommandInfo {
    const char* name;
    const char* args;
    const char* help;
};

constexpr CommandInfo kCommands[] = {
    {"/model",    "[name]",    "show or switch the model"},
    {"/think",    "",          "toggle thinking on/off"},
    {"/yes",      "",          "toggle auto-approve in agent mode"},
    {"/clear",    "",          "start a new conversation"},
    {"/status",   "",          "show all backends"},
    {"/wake",     "<backend>", "wake a backend and start its model"},
    {"/stop",     "<backend>", "stop a backend's model server"},
    {"/poweroff", "<backend>", "power off a backend (asks first)"},
    {"/help",     "",          "show this list"},
    {"/exit",     "",          "quit (or Ctrl+D)"},
};

bool is_command(std::string_view word) {
    return std::any_of(std::begin(kCommands), std::end(kCommands),
                       [&](const CommandInfo& c) { return word == c.name; });
}

void print_help() {
    for (const auto& c : kCommands) {
        std::cout << "  " << out(ansi::cyan, pad(c.name, 10)) << pad(c.args, 11) << out(ansi::dim, c.help) << '\n';
    }
    std::cout << out(ansi::dim, "  Tab completes commands, models and backends · ↑/↓ history · Ctrl+C cancels") << '\n';
}

struct AgentState {
    std::string workspace;      // from the ready event
    bool auto_approve = false;  // /yes toggles it
};

struct ChatState {
    std::string model;
    bool think = false;
    json messages = json::array();
    ModelInfo info;                      // backend of the current model
    int ctx_used = 0;                    // tokens in the context after the last reply
    std::vector<std::string> models;     // for tab completion
    std::vector<std::string> backends;
    std::optional<AgentState> agent;
};

// Updates everything the chat knows from a fresh /admin/status.
void apply_status(ChatState& st, const json& status) {
    st.models = all_models(status);
    st.backends = all_backends(status);
    if (const auto info = find_model(status, st.model)) st.info = *info;
}

std::string shorten_home(std::string path) {
    const char* home_env = std::getenv("HOME");
    if (home_env != nullptr && *home_env != '\0') {
        const std::string home = home_env;
        if (path == home || path.starts_with(home + "/"))
            path.replace(0, home.size(), "~");
    }
    return path;
}

// One line above the prompt: model · backend · thinking · context fill.
void print_status_bar(const ChatState& st) {
    std::string line = out(ansi::dim, "── ") + out(ansi::bold, st.model);
    if (!st.info.backend.empty()) {
        line += out(ansi::dim, " · " + st.info.backend + " ")
              + (st.info.up ? out(ansi::green, "up") : out(ansi::red, "down"));
    }
    if (!st.agent) {
        line += out(ansi::dim, std::string(" · thinking ") + (st.think ? "on" : "off"));
    }
    if (st.agent) {
        std::string workspace = shorten_home(st.agent->workspace);
        line += out(ansi::dim, " · " + workspace)
            + out(ansi::dim, std::string(" · auto-approve "))
            + (st.agent->auto_approve ? out(ansi::red, "ON") : out(ansi::green, "OFF"));
    }

    if (st.info.n_ctx > 0) {
        const int pct = std::min(100, st.ctx_used * 100 / st.info.n_ctx);
        const char* color = pct < 50 ? ansi::green : pct < 80 ? ansi::yellow : ansi::red;
        constexpr int cells = 10;
        const int filled = (pct * cells + 50) / 100;
        std::string bar;
        for (int i = 0; i < cells; ++i) bar += (i < filled) ? "█" : "░";
        line += out(ansi::dim, " · ctx " + short_count(st.ctx_used) + "/" + short_count(st.info.n_ctx) + " ")
              + out(color, bar + " " + std::to_string(pct) + "%");
    } else {
        line += out(ansi::dim, " · ctx " + short_count(st.ctx_used) + "/?");
    }
    std::cout << line << '\n';
}

// Command history on disk. Only /commands are saved: your prompts stay in memory
// for the session (↑ still finds them) but never touch the disk.
std::string history_path() {
    const char* xdg = std::getenv("XDG_STATE_HOME");
    const std::string base = (xdg && *xdg) ? std::string(xdg) : gw::expand_home("~/.local/state");
    return base + "/wakey/history";
}

void load_history(replxx::Replxx& rx, const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) if (!l.empty()) lines.push_back(l);
    const std::size_t keep = 500;   // only the most recent entries
    const std::size_t from = lines.size() > keep ? lines.size() - keep : 0;
    for (std::size_t i = from; i < lines.size(); ++i) rx.history_add(lines[i]);
}

void append_history(const std::string& path, const std::string& line) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);   // private from the start
    if (fd < 0) return;   // history is a convenience; never fail the chat over it
    const std::string entry = line + '\n';
    [[maybe_unused]] const auto n = ::write(fd, entry.data(), entry.size());
    ::close(fd);
}

void setup_replxx(replxx::Replxx& rx, const ChatState& st) {
    using replxx::Replxx;
    rx.install_window_change_handler();
    rx.set_max_history_size(1000);
    rx.set_unique_history(true);
    rx.set_word_break_characters(" \t");
    rx.set_no_color(!g_color_out);
    rx.set_max_hint_rows(1);

    // Tab: complete the command, or its argument (model / backend name).
    rx.set_completion_callback([&st](const std::string& input, int& context_len) {
        Replxx::completions_t result;
        const auto space = input.find(' ');
        if (space == std::string::npos) {
            if (!input.starts_with('/')) return result;
            for (const auto& c : kCommands)
                if (std::string_view(c.name).starts_with(input)) result.emplace_back(c.name);
            context_len = static_cast<int>(input.size());
            return result;
        }
        const std::string cmd = input.substr(0, space);
        const std::string prefix = input.substr(space + 1);
        const std::vector<std::string>* pool = nullptr;
        if (cmd == "/model") pool = &st.models;
        else if (cmd == "/wake" || cmd == "/stop" || cmd == "/poweroff") pool = &st.backends;
        if (pool == nullptr) return result;
        for (const auto& name : *pool)
            if (name.starts_with(prefix)) result.emplace_back(name);
        context_len = static_cast<int>(prefix.size());
        return result;
    });

    // Gray hint after the cursor while typing a command: "/wa" → "ke".
    rx.set_hint_callback([](const std::string& input, int& context_len, Replxx::Color& color) {
        Replxx::hints_t hints;
        if (input.size() < 2 || !input.starts_with('/') || input.find(' ') != std::string::npos) return hints;
        for (const auto& c : kCommands)
            if (std::string_view(c.name).starts_with(input)) hints.emplace_back(c.name);
        context_len = static_cast<int>(input.size());
        color = Replxx::Color::GRAY;
        return hints;
    });

    // Known commands in cyan, unknown /words in red, so typos stand out before Enter.
    rx.set_highlighter_callback([](const std::string& input, Replxx::colors_t& colors) {
        if (input.size() < 2 || !input.starts_with('/')) return;   // a lone "/" isn't a typo yet
        const std::size_t end = std::min(input.find(' '), input.size());
        const auto color = is_command(std::string_view(input).substr(0, end)) ? Replxx::Color::CYAN : Replxx::Color::RED;
        for (std::size_t i = 0; i < end && i < colors.size(); ++i) colors[i] = color;
    });
}

// Handles one /command. Returns false if the chat should end.
bool run_command(const std::string& line, ChatState& st, const gw::ClientConfig& conf, replxx::Replxx& rx) {
    const auto space = line.find(' ');
    const std::string cmd = line.substr(0, space);
    const std::string arg = (space == std::string::npos) ? "" : line.substr(space + 1);
    const bool needs_backend = (cmd == "/wake" || cmd == "/stop" || cmd == "/poweroff");

    if (needs_backend && arg.empty()) {
        std::cerr << "usage: " << cmd << " <backend>\n";
        return true;
    }

    if (cmd == "/exit") return false;
    if (cmd == "/help") { print_help(); return true; }
    if (cmd == "/clear") {
        st.messages = json::array();
        st.ctx_used = 0;
        std::cout << "conversation cleared\n";
        return true;
    }
    if (cmd == "/think") {
        st.think = !st.think;
        std::cout << "thinking " << (st.think ? "on" : "off") << '\n';
        return true;
    }
    if (cmd == "/status") {
        const json status = fetch_status(conf);
        print_status(status);
        apply_status(st, status);
        return true;
    }
    if (st.agent && (cmd == "/think" || cmd == "/model" || cmd == "/clear")) {
        print_error(cmd + " is not available in agent mode");
        return true;
    }
    if (cmd == "/yes") {
        if (!st.agent) { print_error("/yes only works in agent mode"); return true; }
        st.agent->auto_approve = !st.agent->auto_approve;
        std::cout << "auto-approve " << (st.agent->auto_approve ? "on" : "off") << '\n';
        return true;
    }
    if (cmd == "/model") {
        if (arg.empty()) { std::cout << "current model: " << st.model << '\n'; return true; }
        const json status = fetch_status(conf);
        const auto info = find_model(status, arg);
        if (!info) {
            std::cout << "unknown model; available: ";
            for (const auto& m : all_models(status)) std::cout << m << ' ';
            std::cout << '\n';
            return true;
        }
        st.model = arg;
        apply_status(st, status);
        std::cout << "now using " << st.model << '\n';
        if (!info->up) std::cout << out(ansi::yellow, info->backend + " is down, use /wake " + info->backend) << '\n';
        if (!st.messages.empty()) std::cout << out(ansi::dim, "(the conversation so far comes along)") << '\n';
        return true;
    }
    if (cmd == "/wake") {
        auto cli = make_client(conf);
        const json body = with_spinner("waking " + arg, cli, [&] { return post_admin(cli, conf, "/admin/" + arg + "/wake"); });
        print_wake_result(arg, body);
        apply_status(st, fetch_status(conf));   // the backend now reports its context size
        return true;
    }
    if (cmd == "/stop") {
        auto cli = make_client(conf);
        post_admin(cli, conf, "/admin/" + arg + "/stop");
        std::cout << arg << ": model server stopped\n";
        apply_status(st, fetch_status(conf));
        return true;
    }
    if (cmd == "/poweroff") {
        // Ask through replxx, so the answer gets line editing too and isn't added to history.
        const char* answer = rx.input("power off " + arg + "? [y/N] ");
        const std::string a = answer ? answer : "";
        if (a != "y" && a != "Y" && a != "yes" && a != "Yes") { std::cout << "aborted\n"; return true; }
        auto cli = make_client(conf);
        post_admin(cli, conf, "/admin/" + arg + "/poweroff");
        std::cout << arg << " is shutting down\n";
        if (st.info.backend == arg) { st.info.up = false; st.info.n_ctx = 0; }
        return true;
    }

    std::cerr << "unknown command: " << cmd << " (try /help)\n";
    return true;
}

// Sends one user message and prints the reply. Keeps the history consistent on errors.
void run_turn(const std::string& line, ChatState& st, const gw::ClientConfig& conf) {
    st.messages.push_back({{"role", "user"}, {"content", line}});
    auto cli = make_client(conf);
    const auto start = Clock::now();
    try {
        OutputGate gate;
        const StreamResult r = with_spinner(st.think ? "thinking" : "generating", cli,
                                       [&] { return stream_chat(cli, conf, st.model, st.messages, st.think, gate); }, &gate);
        st.ctx_used = r.prompt_tokens + r.completion_tokens;
        const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        std::cout << '\n';
        std::ostringstream s;
        s << r.completion_tokens << " tokens";
        if (r.tps > 0)
            s << " · " << std::fixed << std::setprecision(0) << r.tps << " t/s";
        s << " · " << std::fixed << std::setprecision(1) << seconds << " s";
        std::cerr << err(ansi::dim, s.str()) << '\n';
        st.messages.push_back({{"role", "assistant"}, {"content", r.reply}});
        if (st.info.n_ctx > 0 && st.ctx_used * 100 / st.info.n_ctx >= 80)
            std::cerr << err(ansi::yellow, "context almost full, /clear to start over") << '\n';
    } catch (...) {
        st.messages.erase(st.messages.size() - 1);   // no answer → forget the question too
        throw;
    }
}

std::string preview(const std::string& text, int max_lines = 3) {
    std::istringstream in(text);
    std::string line, shown;
    int n = 0;
    while (std::getline(in, line)) {
        if (n++ == max_lines) { shown += "  …\n"; break; }
        shown += "  " + line + "\n";
    }
    return shown;
}

bool render_event(const json& ev, ChatState& st, gw::AgentProcess& agent) {
    const std::string type = ev.value("type", "");

    if (type == "text") {
        std::cout << ev.value("delta", "") << std::flush;
    } else if (type == "text_end") {
        std::cout << '\n';
    } else if (type == "tool_call") {
        std::cout << out(ansi::dim, "→ " + ev.value("name", "") + "(" + ev.value("args", "").substr(0, 80) + ")") << '\n';
    } else if (type == "approve") {
        const int id = ev.value("id", 0);
        const std::string command = ev.value("command", "");
        bool ok = st.agent->auto_approve;
        if (ok) std::cout << out(ansi::yellow, "auto-approved: " + command) << '\n';
        else    ok = confirm("run '" + command + "'? [y/N]: ");
        agent_send(agent, {{"type", "approval"}, {"id", id}, {"ok", ok}});
    } else if (type == "done") {
        if (ev.value("reason", "") == "cancelled") std::cout << err(ansi::yellow, "cancelled") << '\n';
        return false;
    } else if (type == "tool_result") {
        std::cout << out(ansi::dim, preview(ev.value("output", "")));
    } else if (type == "usage") {
        st.ctx_used = ev.value("used", 0);
        if (st.info.n_ctx == 0) st.info.n_ctx = ev.value("limit", 0);
    } else if (type == "warning") {
        std::cerr << out(ansi::yellow, "warning: " + ev.value("message", "")) << '\n';
    } else if (type == "error") {
        std::string msg = ev.value("message", "");
        if (msg.starts_with("HTTP 503")) msg += " (try /wake <backend>)";
        std::cerr << out(ansi::red, "error: " + msg) << '\n';
    }
    return true;
}


void run_agent_turn(const std::string& line, ChatState& st, gw::AgentProcess& agent) {
    SigintGuard guard;
    bool cancel_sent = false;
    agent_send(agent, {{"type", "user"}, {"content", line}});
    while (true) {
        const gw::NextEvent ev = agent_next_event(agent, 100);
        switch (ev.kind) {
            case gw::NextEvent::Event:
                if (!render_event(ev.event, st, agent)) return;
                break;
            case gw::NextEvent::Timeout:
                if (g_interrupted && !cancel_sent) {
                    kill(agent.pid, SIGINT);
                    cancel_sent = true;
                }
                break;
            case gw::NextEvent::Closed:
                throw gw::AgentGone("agent backend exited (see ~/.local/state/wakey/agent.log)");
        }
    }
}

int cmd_chat(const gw::ClientConfig& conf, const std::string& model, bool think,
            const bool agent_mode, bool yes) {
    ChatState st;
    st.model = model;
    st.think = think;

    // Fail if Wakey is unreachable or the model doesn't exist.
    const json status = fetch_status(conf);
    if (!find_model(status, st.model)) {
        print_error("unknown model '" + st.model + "'");
        std::cerr << "available:";
        for (const auto& m : all_models(status)) std::cerr << ' ' << m;
        std::cerr << '\n';
        return 2;
    }
    apply_status(st, status);

    gw::AgentProcess agent;
    if (agent_mode) {
        signal(SIGPIPE, SIG_IGN);
        st.agent = AgentState{.workspace = "", .auto_approve = yes};
        gw::agent_start(agent, st.model, st.info.n_ctx);

        gw::NextEvent ready;
        do { ready = gw::agent_next_event(agent, -1); } while (ready.kind == gw::NextEvent::Timeout);
        if (ready.kind == gw::NextEvent::Event && ready.event.value("type", "") == "error")
            throw std::runtime_error(ready.event.value("message", "agent backend failed to start"));
        if (ready.kind != gw::NextEvent::Event || ready.event.value("type", "") != "ready")
            throw std::runtime_error("agent backend did not start (see ~/.local/state/wakey/agent.log)");
        st.agent->workspace = ready.event.value("workspace", "");
    }

    replxx::Replxx rx;
    setup_replxx(rx, st);
    const std::string hist = history_path();
    load_history(rx, hist);

    std::string msg = agent_mode ? "wakey agent · working in " + shorten_home(st.agent->workspace) : "wakey chat";
    std::cout << out(ansi::bold, msg) << out(ansi::dim, "  ·  /help for commands  ·  Ctrl+D to quit") << '\n';
    if (!st.info.up)
        std::cout << out(ansi::yellow, st.info.backend + " is down, use /wake " + st.info.backend) << '\n';

    while (true) {
        std::cout << '\n';
        print_status_bar(st);
        const char* raw = rx.input(out(ansi::blue, "❯ "));
        if (raw == nullptr) {
            if (errno == EAGAIN) continue;   // Ctrl+C: discard the line, new prompt
            break;                           // Ctrl+D (or end of piped input): quit
        }
        const std::string line = raw;
        if (line.find_first_not_of(" \t") == std::string::npos) continue;   // empty / only spaces
        rx.history_add(line);

        try {
            if (line.starts_with('/')) {
                append_history(hist, line);
                if (!run_command(line, st, conf, rx)) break;
            } else {
                if (st.agent) run_agent_turn(line, st, agent);
                else run_turn(line, st, conf);
            }
        } catch (const Cancelled&) {
            std::cerr << err(ansi::yellow, "cancelled") << '\n';
        } catch (const gw::AgentGone& e) {
            print_error(e.what());
            return 1;                      // end session
        } catch (const std::exception& e) {
            std::string msg = e.what();
            if (msg.starts_with("HTTP 503") && !st.info.backend.empty()) {
                st.info.up = false;
                msg += " (try /wake " + st.info.backend + ")";
            }
            print_error(msg);
        }
    }
    std::cout << '\n';
    return 0;
}
}  // namespace

// main

int main(int argc, char** argv) {
    g_color_out = isatty(STDOUT_FILENO) && std::getenv("NO_COLOR") == nullptr;
    g_color_err = isatty(STDERR_FILENO) && std::getenv("NO_COLOR") == nullptr;

    if (argc < 2) { print_usage(); return 2; }
    auto arg = [&](int i) -> std::string { return i < argc ? argv[i] : ""; };
    const std::string cmd = arg(1);

    try {
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
            if (yes) ++i;                       // flag consumed -> name moves one position along
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

        if (cmd == "chat" || cmd == "agent") {
            bool think = false, yes = false;
            std::string model;
            for (int i = 2; i < argc; ++i) {
                const std::string a = argv[i];
                if (a == "--think") think = true;
                else if (a == "--yes") yes = true;
                else if (a.starts_with("--")) { print_error("unknown option " + a); return 2; }
                else model = a;
            }
            if (model.empty()) model = conf.default_model;
            return cmd_chat(conf, model, think, cmd == "agent", yes);
        }

        print_usage();
        return 2;
    } catch (const Cancelled&) {
        std::cerr << err(ansi::yellow, "cancelled") << '\n';
        return 130;   // the usual exit code after Ctrl+C
    } catch (const std::exception& e) {
        print_error(e.what());
        return 1;
    }
}

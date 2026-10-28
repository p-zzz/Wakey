#include "agent_process.hpp"
#include "keyfile.hpp"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>


using json = nlohmann::json;

namespace gw {

    // destructor
    AgentProcess::~AgentProcess() {
        if (to_child != -1) close(to_child);
        if (from_child != -1) close(from_child);
        if (pid > 0) {
            int status = 0;
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}   // retry if interrupted
        }
    }

    // Spawns an agent process
    void agent_start(AgentProcess& agent, const std::string& model, int n_ctx){
        const std::string log_path = gw::expand_home("~/.local/state/wakey/agent.log");
        std::filesystem::create_directories(std::filesystem::path(log_path).parent_path());

        std::vector<std::string> args = {"wakey-agent", "--serve", "--model", model};
        if (n_ctx > 0) {
            args.push_back("--context-size");
            args.push_back(std::to_string(n_ctx));
        }
        std::vector<char*> argv;
        for (std::string& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);

        int in_pipe[2];
        if (pipe2(in_pipe, O_CLOEXEC) != 0) {
            throw std::runtime_error(std::string("failed to create in_pipe: ") + std::strerror(errno));
        }
        int out_pipe[2];
        if (pipe2(out_pipe, O_CLOEXEC) != 0) {
            const int e = errno;
            close(in_pipe[0]); close(in_pipe[1]);
            throw std::runtime_error(std::string("failed to create out_pipe: ") + std::strerror(e));
        }

        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, log_path.c_str(),
                                         O_WRONLY | O_CREAT | O_APPEND, 0600);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);

        int spawn_err = posix_spawnp(&agent.pid, "wakey-agent", &fa, &attr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);

        if (spawn_err != 0) {
            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            agent.pid = -1;
            throw std::runtime_error(std::string("cannot start wakey-agent: ") + std::strerror(spawn_err)
                                        + " (is it installed and on PATH?)");
        }

        close(in_pipe[0]);
        close(out_pipe[1]);

        agent.to_child = in_pipe[1];
        agent.from_child = out_pipe[0];
    }

    // Sends text to agent process
    void agent_send(AgentProcess& agent, const json& msg) {
        const std::string text = msg.dump() + '\n';
        const char* p = text.data();
        std::size_t left = text.size();

        while (left > 0) {
            const ssize_t n = ::write(agent.to_child, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("cannot write to agent: ") + std::strerror(errno));
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
    }

    NextEvent agent_next_event(AgentProcess& agent, int timeout_ms) {
        while (true) {
            // Return a complete line without waiting
            const std::size_t pos = agent.buffer.find('\n');
            if (pos != std::string::npos) {
                const std::string line = agent.buffer.substr(0, pos);
                agent.buffer.erase(0, pos + 1);
                auto j = nlohmann::json::parse(line, nullptr, false);
                if (j.is_discarded()) continue;
                return {NextEvent::Event, std::move(j)};
            }

            // Wait for data with timeout_ms
            pollfd p{};
            p.fd = agent.from_child;
            p.events = POLLIN;
            const int r = poll(&p, 1, timeout_ms);
            if (r == 0) return {NextEvent::Timeout, {}};
            if (r < 0) {
                if (errno == EINTR) return {NextEvent::Timeout, {}};
                throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
            }

            // 3. Data (or end of file) available: read once then back to top
            char chunk[4096];
            const ssize_t n = ::read(agent.from_child, chunk, sizeof chunk);
            if (n < 0) {
                if (errno == EINTR) return {NextEvent::Timeout, {}};
                throw std::runtime_error(std::string("cannot read from agent: ") + std::strerror(errno));
            }
            if (n == 0) return {NextEvent::Closed, {}};
            agent.buffer.append(chunk, static_cast<std::size_t>(n));
        }
    }

}   // namespace gw

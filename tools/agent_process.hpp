#pragma once

#include <optional>
#include <string>
#include <nlohmann/json.hpp>
#include <sys/types.h>


namespace gw {

    struct AgentProcess {
        pid_t pid = -1;
        int to_child = -1;
        int from_child = -1;
        std::string buffer;

        ~AgentProcess();                                   // close to_child, waitpid, close from_child
        AgentProcess() = default;
        AgentProcess(const AgentProcess&) = delete;
        AgentProcess& operator=(const AgentProcess&) = delete;
    };

    void agent_start(AgentProcess& agent, const std::string& model, int n_ctx);
    void agent_send(AgentProcess& agent, const nlohmann::json& msg);
    std::optional<nlohmann::json> agent_next_event(AgentProcess& agent);

}   // Namespace gw

#pragma once

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

    struct NextEvent {
        enum Kind { Event, Timeout, Closed } kind;
        nlohmann::json event;
    };

    void agent_start(AgentProcess& agent, const std::string& model, int n_ctx);
    void agent_send(AgentProcess& agent, const nlohmann::json& msg);
    NextEvent agent_next_event(AgentProcess& agent, int timeout_ms);

}   // Namespace gw

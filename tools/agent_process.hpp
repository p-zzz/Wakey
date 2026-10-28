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

        ~AgentProcess();
        AgentProcess() = default;
        AgentProcess(const AgentProcess&) = delete;
        AgentProcess& operator=(const AgentProcess&) = delete;
    };

    struct NextEvent {
        enum Kind { Event, Timeout, Closed } kind;
        nlohmann::json event;
    };

    struct AgentGone : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    void agent_start(AgentProcess& agent, const std::string& model, int n_ctx);
    void agent_send(AgentProcess& agent, const nlohmann::json& msg);
    NextEvent agent_next_event(AgentProcess& agent, int timeout_ms);

}   // Namespace gw

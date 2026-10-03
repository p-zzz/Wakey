#include "power.hpp"
#include <cstring>
#include <iostream>
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <vector>

extern char** environ;

namespace gw {

int run_remote(const std::string& user, const std::string& host,
               const std::string& key_file, const std::string& action) {

    pid_t processID;
    std::vector<std::string> args = {
        "ssh",
        "-i",
        key_file,
        "-o",
        "BatchMode=yes",
        "-o",
        "IdentitiesOnly=yes",
        "-o",
        "ConnectTimeout=5",
        "-o",
        "StrictHostKeyChecking=yes",
        "-n",
        user + "@" + host,
        action
    };

    std::vector<char*> argV{};

    for (std::string& arg : args) {
        argV.push_back(arg.data());
    }
    argV.push_back(nullptr);

    int status = 0;

    const int spawn_err = posix_spawnp(&processID, "ssh", nullptr, nullptr, argV.data(), environ);
    if (spawn_err != 0) {
        std::cerr << "failed to launch ssh: " << std::strerror(spawn_err) << '\n';
        return -1;
    }
    waitpid(processID, &status, 0);
    if (!WIFEXITED(status)) {
        std::cerr << "Launched applicaton\n";
        return -1;
    }
    return WEXITSTATUS(status);
}

} // namespace gw

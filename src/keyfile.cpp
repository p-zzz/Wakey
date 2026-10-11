#include "keyfile.hpp"

#include <stdexcept>
#include <sys/stat.h>
#include <fstream>

namespace gw {
    void check_private_file(const std::string& path) {
        struct stat st{};
        if (stat(path.c_str(), &st) != 0) {
            throw std::runtime_error(path + " : file doesn't exist or can't be accessed");
        }
        if (st.st_mode & (S_IRWXG | S_IRWXO)) {
            throw std::runtime_error("access refused; to make your key private run: chmod 600 <path>");
        }
    }


    std::string read_key_file(const std::string& path) {

        check_private_file(path);

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


    std::string expand_home(std::string path) {
        if (path.starts_with("~/")) {
            const char* home = std::getenv("HOME");
            if (home == nullptr) throw std::runtime_error("HOME is not set");
            path.replace(0, 1, home);
        }
        return path;
    }
}

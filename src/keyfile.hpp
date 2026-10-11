#pragma once
#include <string>

namespace gw {

void check_private_file(const std::string& path);
std::string read_key_file(const std::string& path);
std::string expand_home(std::string path);

}   // namespace gw

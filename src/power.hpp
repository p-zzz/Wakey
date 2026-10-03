#pragma once

#include <string>


namespace gw {

int run_remote(const std::string& user, const std::string& host,
               const std::string& key_file, const std::string& action);

} // namespace gw

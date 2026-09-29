#include "auth.hpp"

namespace gw {

bool constant_time_equal(const std::string& a, const std::string& b) {
    if (!(a.size() == b.size())) return false;

    int diff = 0;
    for (unsigned int i = 0; i < a.size(); i++) {
        diff |= a[i] ^ b[i];    // XOR
    }
    return (diff == 0) ? true : false;
}

}   // namespace gw

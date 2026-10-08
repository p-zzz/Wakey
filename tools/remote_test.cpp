#include "power.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc == 5) {
        int res = gw::run_remote(argv[1], argv[2], argv[3], argv[4]);
        std::cout << "Result: " << res << '\n';
        return 0;
    }
    return 1;
}

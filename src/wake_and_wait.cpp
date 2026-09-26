#include "health.hpp"
#include "wol.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <string>
#include <system_error>

namespace {
    bool parse_int(const std::string& text, int& out){
        int value = 0;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec == std::errc{} && ptr == text.data() + text.size()){
           out = value;
           return true;
        }
        return false;
    }
}


int main(int argc, char** argv){
    if (argc < 4 || argc > 5) {
        std::cerr << "usage: " << argv[0] << " <MAC> <host> <port> [timeout_s]\n";
        return 2;
    }

    int timeout = 180;

    const std::string mac_text = argv[1];
    const std::string host = argv[2];
    const std::string port_text = argv[3];
    if (argc == 5) {
        const std::string timeout_text = argv[4];    // If optional argument

        if (!parse_int(timeout_text, timeout) || timeout < 1) {
            std::cerr << "invalid timeout " << timeout_text << " (must be positive)\n";
            return 2;
        }
    }
    // Create and parse MAC
    gw::MacAddress mac{};
    if (!gw::parse_mac_addr(mac_text, mac)){
        std::cerr << "invalid MAC: " << mac_text << " (must be <AA:BB:CC:DD:EE:FF>)\n";
        return 2;
    }
    // Parse port
    int port = 0;
    if (!parse_int(port_text, port) || port < 1 || port > 65535) {
        std::cerr << "invalid port: " << port_text << " (must be 1-65535)\n";
        return 2;
    }
    // Check if host is up
    if (gw::is_healthy(host, port)) {
        std::cout << "backend already up\n";
        return 0;
    }

    const auto start = std::chrono::steady_clock::now();
    // Send magic packet
    if (!gw::send_magic_packet(mac, "192.168.1.255")) return 1;

    // Wait til host is healthy
    const bool up = gw::wait_until_healthy(host, port, std::chrono::seconds(timeout));
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!up) {
        std::cerr << "timed out after " << elapsed << " s\n";
        return 1;
    }

    std::cout << "backend ready after " << elapsed << " s\n";
    return 0;
}

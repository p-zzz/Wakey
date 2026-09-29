#include "wol.hpp"
#include <iostream>
#include <cstdio>
#include <cstddef>
#include <iomanip>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <charconv>
#include <system_error>

namespace gw {

    namespace {
        struct SocketGuard {
            int fd;                                     // Data member
            explicit SocketGuard(int f) : fd(f) {}      // Constructor runs when object is created
            ~SocketGuard(){                             // Destructor runs when object goes out of scope
                if (fd >= 0) close(fd);
            }

            SocketGuard(const SocketGuard&) = delete;   // forbid copy construction
            SocketGuard& operator=(const SocketGuard&) = delete;   // no copy assignment
        };
    }

    // BUILD
    MagicPacket build_magic_packet(const MacAddress& mac){
        MagicPacket p = {};
        // Fill in first 6 bytes
        for (int i = 0; i < 6; i++){
            p[i] = 0xFF;
        }
        // Fill in the rest
        for (int i = 0; i < 16; i++){
            for (int j = 0; j < 6; j++){
            p[6 + 6*i + j] = mac[j];
            }
        }
        return p;
    }

    // PRINT
    void print_magic_packet(const MagicPacket& p){
        for (std::size_t i = 0; i < p.size(); i++){
            std::cout << std::setw(2) << std::setfill('0') << std::hex << static_cast<int>(p[i]);
            std::cout << (i % 6 == 5 ? '\n' : ':');
        }
        std::cout << std::dec << std::setfill(' ') << '\n';
    }

    // SEND
    bool send_magic_packet(const MacAddress& mac, const std::string& broadcast){
        MagicPacket p = build_magic_packet(mac);

        SocketGuard sock(socket(AF_INET, SOCK_DGRAM, 0));
        if (sock.fd < 0) {
            perror("socket");
            return false;
        }
        int enable = 1;
        if (setsockopt(sock.fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0){
            perror("setsockopt");
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9);
        /*inet_pton sets sin_addr and if statement checks the status */
        if (inet_pton(AF_INET, broadcast.c_str(), &addr.sin_addr) != 1) {
            std::cerr << "invalid broadcast address\n";
            return false;
        }
        // sendto
        if (sendto(sock.fd, p.data(), p.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
            perror("sendto");
            return false;
        }
        return true;
    }

    bool parse_mac_addr(const std::string& text, MacAddress& out){
        MacAddress mac{};
        if (text.size() != 17) return false;
        for (int i = 0; i < 6; i++){
            unsigned int value = 0;
            const char sep = text[i * 3 + 2];
            if (i != 5 && sep != ':' && sep != '-') {
                if (i != 5) return false;
            }
            const char* start = text.data() + i * 3;
            auto [ptr, ec] = std::from_chars(start, start + 2, value, 16);
            if (ec != std::errc{} || ptr != start + 2) return false;
            mac[i] = static_cast<std::uint8_t>(value);
        }
        out = mac;
        return true;
    }

}

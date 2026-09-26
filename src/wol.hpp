#pragma once    // paste this file only once per .cpp
#include <array>
#include <cstdint>
#include <string>

namespace gw{

    using MacAddress = std::array<std::uint8_t, 6>;
    using MagicPacket = std::array<std::uint8_t, 102>;

    MagicPacket build_magic_packet(const MacAddress& mac);
    void print_magic_packet(const MagicPacket& p);
    bool send_magic_packet(const MacAddress& mac, const std::string& broadcast);
    bool parse_mac_addr(const std::string& text, MacAddress& out);
}

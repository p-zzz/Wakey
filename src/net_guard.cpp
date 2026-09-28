#include "net_guard.hpp"
#include <arpa/inet.h>
#include <charconv>
#include <netinet/in.h>
#include <optional>
#include <array>
#include <sys/types.h>

namespace gw {

namespace {
    const std::array<Ipv4Cidr, 4> kPrivateRanges = {{
        {0x0A000000, 8},    // 10.0.0.0/8
        {0xAC100000, 12},   // 172.16.0.0/12
        {0xC0A80000, 16},   // 192.168.0.0/16
        {0x7F000000, 8},    // 127.0.0.0/8 (loopback)
    }};
}

std::optional<std::uint32_t> parse_ipv4(const std::string &text) {
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1) return std::nullopt;

    std::uint32_t ip = ntohl(addr.s_addr);
    return ip;
}

std::optional<Ipv4Cidr> parse_cidr(const std::string& text){
    // separate network from prefix '/'
    Ipv4Cidr ipv4;

    const auto pos = text.find('/');
    if (pos == std::string::npos) return std::nullopt;

    int value;
    const std::string submask = text.substr(pos + 1);
    auto [ptr, ec] = std::from_chars(submask.data(), submask.data() + submask.size(), value);
    if (ec == std::errc{} && ptr == submask.data() + submask.size() && value >= 0 && value <= 32){
        ipv4.prefix = value;
    } else return std::nullopt;

    const std::string net = text.substr(0, pos);
    const auto ip = parse_ipv4(net);
    if (!ip) return std::nullopt;
    ipv4.network = *ip;
    //
    if (ipv4.contains(ipv4.network)) return ipv4;

    return std::nullopt;
}

bool is_private(const Ipv4Cidr& range) {
    for (const Ipv4Cidr& p : kPrivateRanges) {
        if (p.contains(range.network) && range.prefix >= p.prefix) {
            return true;
        }
    }
    return false;
}

} // namespace gw

#pragma once
#include <cstdint>
#include <optional>
#include <string>


namespace gw {

    struct Ipv4Cidr {
        std::uint32_t network = 0;
        int prefix = 0;

        std::uint32_t mask() const { return prefix == 0 ? 0 : ~std::uint32_t{0} << (32 - prefix); }
        bool contains(std::uint32_t ip) const { return (ip & mask()) == network; }
    };

    std::optional<std::uint32_t> parse_ipv4(const std::string& text);
    std::optional<Ipv4Cidr> parse_cidr(const std::string& text);

}   // namespace gw

#include <iostream>

#include "net_guard.hpp"

namespace {

int failures = 0;

void check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

// True if `cidr` and `ip` both parse and the address lies inside the range.
bool in_range(const char* cidr, const char* ip) {
    const auto range = gw::parse_cidr(cidr);
    const auto addr = gw::parse_ipv4(ip);
    return range && addr && range->contains(*addr);
}

}  // namespace

int main() {
    // parse_ipv4
    check(gw::parse_ipv4("192.168.1.171").has_value(), "accepts 192.168.1.171");
    check(gw::parse_ipv4("192.168.1.171") == 0xC0A801ABu, "192.168.1.171 parses to 0xC0A801AB");
    check(!gw::parse_ipv4("banana").has_value(), "rejects banana as an address");
    check(!gw::parse_ipv4("256.0.0.1").has_value(), "rejects 256.0.0.1 (octet too big)");
    check(!gw::parse_ipv4("").has_value(), "rejects empty address");

    // parse_cidr: should accept
    check(gw::parse_cidr("10.127.210.0/24").has_value(), "accepts 10.127.210.0/24");
    check(gw::parse_cidr("0.0.0.0/0").has_value(), "accepts 0.0.0.0/0");
    check(gw::parse_cidr("10.127.210.5/32").has_value(), "accepts 10.127.210.5/32");

    // parse_cidr: should reject
    check(!gw::parse_cidr("banana/24").has_value(), "rejects banana/24 (invalid address)");
    check(!gw::parse_cidr("10.0.0.0/33").has_value(), "rejects /33 (prefix too big)");
    check(!gw::parse_cidr("10.0.0.0/-1").has_value(), "rejects /-1 (negative prefix)");
    check(!gw::parse_cidr("10.0.0.0/24abc").has_value(), "rejects /24abc (trailing garbage)");
    check(!gw::parse_cidr("10.0.0.0/").has_value(), "rejects empty prefix");
    check(!gw::parse_cidr("10.0.0.0").has_value(), "rejects missing slash");
    check(!gw::parse_cidr("/24").has_value(), "rejects missing address");
    check(!gw::parse_cidr("10.127.210.5/24").has_value(), "rejects host bits set in 10.127.210.5/24");

    // contains: /24
    check(in_range("10.127.210.0/24", "10.127.210.5"), "10.127.210.5 is inside 10.127.210.0/24");
    check(in_range("10.127.210.0/24", "10.127.210.0"), "first address is inside 10.127.210.0/24");
    check(in_range("10.127.210.0/24", "10.127.210.255"), "last address is inside 10.127.210.0/24");
    check(!in_range("10.127.210.0/24", "10.127.211.5"), "10.127.211.5 is outside 10.127.210.0/24");
    check(!in_range("10.127.210.0/24", "10.127.209.255"), "address just below is outside 10.127.210.0/24");

    // contains: /0 (everything) and /32 (exactly one)
    check(in_range("0.0.0.0/0", "8.8.8.8"), "/0 contains 8.8.8.8");
    check(in_range("0.0.0.0/0", "10.1.2.3"), "/0 contains 10.1.2.3");
    check(in_range("1.1.1.1/32", "1.1.1.1"), "/32 contains its own address");
    check(!in_range("1.1.1.1/32", "1.1.1.2"), "/32 does not contain the next address");

    // contains: ranges that don't fall on a byte boundary
    check(in_range("172.16.0.0/12", "172.31.255.255"), "172.31.255.255 is inside 172.16.0.0/12");
    check(!in_range("172.16.0.0/12", "172.32.0.0"), "172.32.0.0 is outside 172.16.0.0/12");

    if (failures == 0) {
        std::cout << "all net_guard tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}

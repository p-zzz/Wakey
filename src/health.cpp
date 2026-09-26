#include "health.hpp"
#include <httplib.h>
#include <ostream>
#include <thread>


namespace gw {
    bool is_healthy(const std::string& host, int port){
        httplib::Client client(host, port);
        client.set_connection_timeout(std::chrono::seconds(1));  // time to establish the connection
        client.set_read_timeout(std::chrono::seconds(2));        // time to wait for the answer once connected
        auto res = client.Get("/health");   // res behaves a lot like a pointer
        return res && res->status == 200;
    }

    bool wait_until_healthy(const std::string& host, int port, std::chrono::seconds timeout){
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline){
            if (is_healthy(host, port)) return true;
            std::cout << '.' << std::flush;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return false;
    }
}

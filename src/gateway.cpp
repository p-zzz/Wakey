#include <httplib.h>
#include <iostream>

/* Graceful shutdown
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        std::cout << "shutting down gracefully...\n";
        svr.stop();
    }
}
*/

int main(){
    // Health check
    httplib::Server svr;
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    if(!svr.bind_to_port("127.0.0.1", 8100)) {
        std::cerr << "failed to bind 127.0.0.1:8100\n";
        return 1;
    }

    std::cout << "server listening on 127.0.0.1:8100\n";
    return svr.listen_after_bind() ? 0 : 1;
}

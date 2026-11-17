#include "server.hpp"
#include <charconv>
#include <exception>
#include <iostream>
#include <string_view>
#include <thread>

int main(int argc, char** argv) {
    unsigned port = 8080;
    unsigned workers = std::max(1u, std::thread::hardware_concurrency());

    if (argc > 3) { std::cerr << "Usage: epoll_http [port] [workers]\n"; return 1; }

    auto number = [](std::string_view s, unsigned& out) {
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
        return ec == std::errc{} && ptr == s.data() + s.size();
    };


    if ((argc > 1 && (!number(argv[1], port) || port < 1 || port > 65535)) ||
        (argc > 2 && (!number(argv[2], workers) || workers < 1 || workers > 256))) {
        std::cerr << "Invalid arguments: port 1..65535, workers 1..256\n"; return 1;
    }

    try {
        Server server(static_cast<std::uint16_t>(port), workers);
        std::cout << "Listening on 0.0.0.0:" << port << " with " << workers << " workers\n";
        server.run();
    } catch (const std::exception& e) { std::cerr << "Error: " << e.what() << '\n'; return 1; }
}

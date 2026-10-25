#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include "slew/server/server.hpp"

static std::atomic<slew::Server*> g_server{nullptr};
static std::atomic<int> g_signal_count{0};

static void signal_handler(int sig) {
    int count = ++g_signal_count;
    if (count > 1) {
        // Immediate termination if signaled again while shutting down
        std::exit(128 + sig);
    }
    if (auto* s = g_server.load(std::memory_order_acquire)) {
        s->request_stop();
    }
}

static void print_usage(const char* prog) {
    std::cout << "slew - Highly Optimised HTTP/1.1 Web Server (C++20, io_uring, Zero Dependencies)\n\n"
              << "Usage: " << prog << " [options]\n\n"
              << "Options:\n"
              << "  -p, --port <port>       Port to listen on (default: 8080)\n"
              << "  -t, --threads <num>     Number of worker threads (default: 0 = all CPU cores)\n"
              << "  -d, --docroot <path>    Document root for static files (default: ./public)\n"
              << "  -s, --timeout <sec>     Idle/read timeout in seconds (default: 5)\n"
              << "  -h, --help              Show this help message\n";
}

int main(int argc, char* argv[]) {
    slew::ServerConfig config;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            config.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if ((arg == "-t" || arg == "--threads") && i + 1 < argc) {
            config.threads = static_cast<size_t>(std::stoul(argv[++i]));
        } else if ((arg == "-d" || arg == "--docroot") && i + 1 < argc) {
            config.docroot = argv[++i];
        } else if ((arg == "-s" || arg == "--timeout") && i + 1 < argc) {
            config.timeout_seconds = std::stoi(argv[++i]);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    slew::Server server(config);
    g_server.store(&server);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN); // Ignore SIGPIPE for socket disconnects

    // Register dynamic demo routes
    server.router().get("/hello", [](const slew::HttpRequest&, slew::HttpResponse& resp) {
        resp.set_status(slew::HttpStatus::OK);
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        resp.set_body("Hello World!");
    });

    server.router().get("/health", [](const slew::HttpRequest&, slew::HttpResponse& resp) {
        resp.set_status(slew::HttpStatus::OK);
        resp.add_header("Content-Type", "application/json");
        resp.set_body("{\"status\":\"ok\",\"server\":\"slew/1.1\"}");
    });

    server.router().post("/api/echo", [](const slew::HttpRequest& req, slew::HttpResponse& resp) {
        resp.set_status(slew::HttpStatus::OK);
        resp.add_header("Content-Type", "text/plain; charset=utf-8");
        // Reuse req.body
        resp.set_body(req.body);
    });

    server.start();
    server.wait();

    std::cout << "\nShutting down gracefully..." << std::endl;
    g_server.store(nullptr, std::memory_order_release);

    return 0;
}

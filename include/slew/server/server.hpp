#pragma once

#include <vector>
#include <thread>
#include <memory>
#include <csignal>
#include <atomic>
#include <chrono>
#include <pthread.h>
#include <iostream>
#include "worker.hpp"
#include "../http/router.hpp"

namespace slew {

struct ServerConfig {
    uint16_t port{8080};
    size_t threads{0}; // 0 = auto-detect hardware concurrency
    std::string docroot{"./public"};
    int timeout_seconds{5};
};

class Server {
public:
    explicit Server(ServerConfig config = {}) : config_(std::move(config)) {
        if (config_.threads == 0) {
            config_.threads = std::thread::hardware_concurrency();
            if (config_.threads == 0) config_.threads = 1;
        }
        std::signal(SIGUSR1, [](int) {});
    }

    ~Server() {
        stop();
    }

    Router& router() noexcept { return router_; }
    const Router& router() const noexcept { return router_; }

    void start() {
        std::cout << "Starting slew HTTP/1.1 Web Server" << std::endl;
        std::cout << "  Port:            " << config_.port << std::endl;
        std::cout << "  Worker Threads:  " << config_.threads << " (pinned to CPU cores)" << std::endl;
        std::cout << "  Document Root:   " << config_.docroot << std::endl;
        std::cout << "  Idle Timeout:    " << config_.timeout_seconds << "s" << std::endl;
        std::cout << "  Architecture:    io_uring (raw syscalls) + Shared-Nothing Thread-Per-Core" << std::endl;

        stopping_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);

        workers_.reserve(config_.threads);
        threads_.reserve(config_.threads);

        for (size_t i = 0; i < config_.threads; ++i) {
            auto worker = std::make_unique<Worker>(i, config_.port, config_.docroot, router_, config_.timeout_seconds);
            workers_.push_back(std::move(worker));
        }

        for (size_t i = 0; i < config_.threads; ++i) {
            threads_.emplace_back([this, i]() {
                workers_[i]->run();
            });
        }
    }

    void request_stop() noexcept {
        running_.store(false, std::memory_order_release);
    }

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    void stop() {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true)) {
            return;
        }
        running_.store(false, std::memory_order_release);

        for (auto& worker : workers_) {
            if (worker) {
                worker->stop();
            }
        }

        // Interrupt any worker thread waiting inside io_uring_enter
        for (auto& t : threads_) {
            if (t.joinable()) {
                pthread_kill(t.native_handle(), SIGUSR1);
            }
        }

        auto current_id = std::this_thread::get_id();
        for (auto& t : threads_) {
            if (t.joinable() && t.get_id() != current_id) {
                t.join();
            }
        }
        threads_.clear();
        workers_.clear();
    }

    void wait() {
        while (running_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        stop();
    }

private:
    ServerConfig config_;
    Router router_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::thread> threads_;
};

}

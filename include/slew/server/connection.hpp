#pragma once

#include <netinet/in.h>
#include <cstdint>
#include <cstring>
#include "../http/http_types.hpp"

namespace slew {

struct Connection {
    static constexpr size_t HEADER_BUFFER_SIZE = 4096;

    int fd{-1};
    struct sockaddr_in client_addr{};
    socklen_t addr_len{sizeof(sockaddr_in)};
    int buffer_id{-1};
    bool in_use{false};
    uint32_t pool_index{0};
    uint32_t last_active_sec{0};

    // Preallocated buffer for serializing response headers
    char resp_header_buf[HEADER_BUFFER_SIZE]{};

    void reset() noexcept {
        fd = -1;
        std::memset(&client_addr, 0, sizeof(client_addr));
        addr_len = sizeof(sockaddr_in);
        buffer_id = -1;
        in_use = false;
        last_active_sec = 0;
    }
};

// O(1) LIFO-ordered connection pool for optimal L1/L2 cache locality
class ConnectionPool {
public:
    explicit ConnectionPool(size_t capacity = 16384)
        : capacity_(capacity),
          pool_(new Connection[capacity]),
          free_stack_(new uint32_t[capacity]),
          free_count_(capacity) {
        for (size_t i = 0; i < capacity_; ++i) {
            pool_[i].reset();
            pool_[i].pool_index = static_cast<uint32_t>(i);
            // Initialize LIFO stack so lowest indices are acquired first
            free_stack_[i] = static_cast<uint32_t>(capacity_ - 1 - i);
        }
    }

    ~ConnectionPool() {
        delete[] free_stack_;
        delete[] pool_;
    }

    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    [[nodiscard]] Connection* acquire() noexcept {
        if (free_count_ == 0) [[unlikely]] {
            return nullptr; // Connection pool saturated
        }
        uint32_t idx = free_stack_[--free_count_];
        pool_[idx].in_use = true;
        return &pool_[idx];
    }

    void release(Connection* conn) noexcept {
        if (!conn || !conn->in_use) [[unlikely]] return;
        uint32_t idx = conn->pool_index;
        if (idx < capacity_ && &pool_[idx] == conn) [[likely]] {
            conn->reset();
            conn->pool_index = idx;
            free_stack_[free_count_++] = idx;
        }
    }

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] size_t active_count() const noexcept { return capacity_ - free_count_; }

    [[nodiscard]] Connection* get_at(size_t index) noexcept {
        return (index < capacity_) ? &pool_[index] : nullptr;
    }

private:
    size_t capacity_;
    Connection* pool_;
    uint32_t* free_stack_;
    size_t free_count_;
};

}

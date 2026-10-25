#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <memory>
#include <atomic>
#include "ring.hpp"

namespace slew {

class ProvidedBufferPool {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 4096;
    static constexpr size_t DEFAULT_BUFFER_COUNT = 1024;
    static constexpr uint16_t DEFAULT_GROUP_ID = 1;

    ProvidedBufferPool() noexcept = default;
    ~ProvidedBufferPool() {
        cleanup();
    }

    ProvidedBufferPool(const ProvidedBufferPool&) = delete;
    ProvidedBufferPool& operator=(const ProvidedBufferPool&) = delete;

    ProvidedBufferPool(ProvidedBufferPool&& other) noexcept {
        move_from(std::move(other));
    }

    ProvidedBufferPool& operator=(ProvidedBufferPool&& other) noexcept {
        if (this != &other) {
            cleanup();
            move_from(std::move(other));
        }
        return *this;
    }

    Result<void> init(IoRing& ring,
                      size_t buffer_count = DEFAULT_BUFFER_COUNT,
                      size_t buffer_size = DEFAULT_BUFFER_SIZE,
                      uint16_t group_id = DEFAULT_GROUP_ID) noexcept {
        cleanup();

        ring_fd_ = ring.fd();
        buffer_count_ = buffer_count;
        buffer_size_ = buffer_size;
        group_id_ = group_id;

        size_t total_bytes = buffer_count_ * buffer_size_;
        // 4096-byte aligned memory for direct kernel I/O
        int ret = ::posix_memalign(&pool_memory_, 4096, total_bytes);
        if (ret != 0 || !pool_memory_) {
            return StatusCode::OutOfMemory;
        }

        std::memset(pool_memory_, 0, total_bytes);

        // Try modern Linux 5.19+ Provided Buffer Ring (IORING_REGISTER_PBUF_RING)
        // Requires buffer_count to be a power of 2
        if ((buffer_count_ & (buffer_count_ - 1)) == 0 && ring_fd_ >= 0) {
            buf_ring_size_ = buffer_count_ * sizeof(struct io_uring_buf);
            void* ring_mem = nullptr;
            int ring_alloc_ret = ::posix_memalign(&ring_mem, 4096, buf_ring_size_);
            if (ring_alloc_ret == 0 && ring_mem) {
                buf_ring_ = static_cast<struct io_uring_buf_ring*>(ring_mem);
                std::memset(buf_ring_, 0, buf_ring_size_);

                for (size_t i = 0; i < buffer_count_; ++i) {
                    buf_ring_->bufs[i].addr = reinterpret_cast<uint64_t>(get_buffer(static_cast<int>(i)));
                    buf_ring_->bufs[i].len = static_cast<uint32_t>(buffer_size_);
                    buf_ring_->bufs[i].bid = static_cast<uint16_t>(i);
                    buf_ring_->bufs[i].resv = 0;
                }
                buf_ring_->tail = static_cast<uint16_t>(buffer_count_);
                buf_ring_tail_ = static_cast<uint16_t>(buffer_count_);

                struct io_uring_buf_reg reg{};
                reg.ring_addr = reinterpret_cast<uint64_t>(buf_ring_);
                reg.ring_entries = static_cast<uint32_t>(buffer_count_);
                reg.bgid = group_id_;
                reg.flags = 0;

                int reg_ret = sys_io_uring_register(ring_fd_, IORING_REGISTER_PBUF_RING, &reg, 1);
                if (reg_ret == 0) {
                    is_pbuf_ring_ = true;
                    return {};
                }

                // Kernel does not support PBUF_RING or rejected, fallback to legacy
                std::free(buf_ring_);
                buf_ring_ = nullptr;
            }
        }

        // Fallback: Legacy IORING_OP_PROVIDE_BUFFERS
        auto* sqe = ring.get_sqe();
        if (!sqe) {
            cleanup();
            return StatusCode::SystemError;
        }

        IoRing::prep_provide_buffers(sqe, pool_memory_, static_cast<int>(buffer_size_),
                                     static_cast<int>(buffer_count_), group_id_, 0, 0);

        int submit_res = ring.submit_and_wait(1);
        if (submit_res < 0) {
            cleanup();
            return StatusCode::SystemError;
        }

        // Consume the CQE for provide_buffers
        struct io_uring_cqe* cqe = ring.peek_cqe();
        if (cqe) {
            if (cqe->res < 0) {
                ring.advance_cq(1);
                cleanup();
                return StatusCode::SystemError;
            }
            ring.advance_cq(1);
        }

        return {};
    }

    [[nodiscard]] uint8_t* get_buffer(int buffer_id) noexcept {
        if (buffer_id < 0 || static_cast<size_t>(buffer_id) >= buffer_count_) {
            return nullptr;
        }
        return static_cast<uint8_t*>(pool_memory_) + (buffer_id * buffer_size_);
    }

    [[nodiscard]] const uint8_t* get_buffer(int buffer_id) const noexcept {
        if (buffer_id < 0 || static_cast<size_t>(buffer_id) >= buffer_count_) {
            return nullptr;
        }
        return static_cast<const uint8_t*>(pool_memory_) + (buffer_id * buffer_size_);
    }

    // Return buffer to io_uring ring
    void return_buffer(IoRing& ring, int buffer_id) noexcept {
        if (buffer_id < 0 || static_cast<size_t>(buffer_id) >= buffer_count_) [[unlikely]] {
            return;
        }

        // Fast path: Zero-syscall userspace return via io_uring_buf_ring
        if (is_pbuf_ring_) [[likely]] {
            uint16_t mask = static_cast<uint16_t>(buffer_count_ - 1);
            uint16_t tail = buf_ring_tail_;
            struct io_uring_buf* b = &buf_ring_->bufs[tail & mask];
            b->addr = reinterpret_cast<uint64_t>(get_buffer(buffer_id));
            b->len = static_cast<uint32_t>(buffer_size_);
            b->bid = static_cast<uint16_t>(buffer_id);
            b->resv = 0;
            buf_ring_tail_ = tail + 1;
            std::atomic_ref(buf_ring_->tail).store(buf_ring_tail_, std::memory_order_release);
            return;
        }

        // Fallback: SQE-based IORING_OP_PROVIDE_BUFFERS
        uint8_t* buf_addr = get_buffer(buffer_id);
        auto* sqe = ring.get_sqe();
        if (!sqe) [[unlikely]] {
            ring.submit();
            sqe = ring.get_sqe();
            if (!sqe) return;
        }
        IoRing::prep_provide_buffers(sqe, buf_addr, static_cast<int>(buffer_size_), 1, group_id_, buffer_id, 0);
    }

    [[nodiscard]] uint16_t group_id() const noexcept { return group_id_; }
    [[nodiscard]] size_t buffer_size() const noexcept { return buffer_size_; }
    [[nodiscard]] size_t buffer_count() const noexcept { return buffer_count_; }
    [[nodiscard]] bool is_pbuf_ring() const noexcept { return is_pbuf_ring_; }

private:
    void cleanup() noexcept {
        if (is_pbuf_ring_ && buf_ring_ && ring_fd_ >= 0) {
            struct io_uring_buf_reg reg{};
            reg.bgid = group_id_;
            sys_io_uring_register(ring_fd_, IORING_UNREGISTER_PBUF_RING, &reg, 1);
        }
        if (buf_ring_) {
            std::free(buf_ring_);
            buf_ring_ = nullptr;
        }
        if (pool_memory_) {
            std::free(pool_memory_);
            pool_memory_ = nullptr;
        }
        buffer_count_ = 0;
        buffer_size_ = 0;
        is_pbuf_ring_ = false;
        ring_fd_ = -1;
    }

    void move_from(ProvidedBufferPool&& other) noexcept {
        pool_memory_ = other.pool_memory_;
        buf_ring_ = other.buf_ring_;
        buf_ring_size_ = other.buf_ring_size_;
        buf_ring_tail_ = other.buf_ring_tail_;
        is_pbuf_ring_ = other.is_pbuf_ring_;
        ring_fd_ = other.ring_fd_;
        buffer_count_ = other.buffer_count_;
        buffer_size_ = other.buffer_size_;
        group_id_ = other.group_id_;

        other.pool_memory_ = nullptr;
        other.buf_ring_ = nullptr;
        other.buf_ring_size_ = 0;
        other.buf_ring_tail_ = 0;
        other.is_pbuf_ring_ = false;
        other.ring_fd_ = -1;
        other.buffer_count_ = 0;
        other.buffer_size_ = 0;
    }

    void* pool_memory_{nullptr};
    struct io_uring_buf_ring* buf_ring_{nullptr};
    size_t buf_ring_size_{0};
    uint16_t buf_ring_tail_{0};
    bool is_pbuf_ring_{false};
    int ring_fd_{-1};
    size_t buffer_count_{0};
    size_t buffer_size_{0};
    uint16_t group_id_{DEFAULT_GROUP_ID};
};

}

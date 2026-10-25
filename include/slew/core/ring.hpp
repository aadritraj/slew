#pragma once

#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <system_error>
#include <functional>
#include "types.hpp"

namespace slew {

inline int sys_io_uring_setup(unsigned int entries, struct io_uring_params* p) noexcept {
    return static_cast<int>(::syscall(__NR_io_uring_setup, entries, p));
}

inline int sys_io_uring_enter(int fd, unsigned int to_submit, unsigned int min_complete,
                              unsigned int flags, sigset_t* sig) noexcept {
    return static_cast<int>(::syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, sig ? sizeof(sigset_t) : 0));
}

inline int sys_io_uring_register(int fd, unsigned int opcode, const void* arg, unsigned int nr_args) noexcept {
    return static_cast<int>(::syscall(__NR_io_uring_register, fd, opcode, arg, nr_args));
}

class IoRing {
public:
    IoRing() noexcept = default;

    ~IoRing() {
        destroy();
    }

    // Move-only
    IoRing(const IoRing&) = delete;
    IoRing& operator=(const IoRing&) = delete;

    IoRing(IoRing&& other) noexcept {
        move_from(std::move(other));
    }

    IoRing& operator=(IoRing&& other) noexcept {
        if (this != &other) {
            destroy();
            move_from(std::move(other));
        }
        return *this;
    }

    Result<void> init(unsigned int entries, unsigned int flags = 0) noexcept {
        destroy();

        std::memset(&params_, 0, sizeof(params_));
        params_.flags = flags;

        ring_fd_ = sys_io_uring_setup(entries, &params_);
        if (ring_fd_ < 0) {
            return StatusCode::SystemError;
        }

        size_t sq_ring_sz = params_.sq_off.array + params_.sq_entries * sizeof(__u32);
        size_t cq_ring_sz = params_.cq_off.cqes + params_.cq_entries * sizeof(struct io_uring_cqe);

        if (params_.features & IORING_FEAT_SINGLE_MMAP) {
            size_t ring_sz = (sq_ring_sz > cq_ring_sz) ? sq_ring_sz : cq_ring_sz;
            sq_ring_ptr_ = ::mmap(nullptr, ring_sz, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
            if (sq_ring_ptr_ == MAP_FAILED) {
                destroy();
                return StatusCode::SystemError;
            }
            sq_ring_sz_ = ring_sz;
            cq_ring_ptr_ = sq_ring_ptr_;
            cq_ring_sz_ = 0; // indicates single mmap
        } else {
            sq_ring_ptr_ = ::mmap(nullptr, sq_ring_sz, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQ_RING);
            if (sq_ring_ptr_ == MAP_FAILED) {
                destroy();
                return StatusCode::SystemError;
            }
            sq_ring_sz_ = sq_ring_sz;

            cq_ring_ptr_ = ::mmap(nullptr, cq_ring_sz, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_CQ_RING);
            if (cq_ring_ptr_ == MAP_FAILED) {
                destroy();
                return StatusCode::SystemError;
            }
            cq_ring_sz_ = cq_ring_sz;
        }

        size_t sqes_sz = params_.sq_entries * sizeof(struct io_uring_sqe);
        sqes_ptr_ = ::mmap(nullptr, sqes_sz, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_POPULATE, ring_fd_, IORING_OFF_SQES);
        if (sqes_ptr_ == MAP_FAILED) {
            destroy();
            return StatusCode::SystemError;
        }
        sqes_sz_ = sqes_sz;

        auto* sq_bytes = static_cast<uint8_t*>(sq_ring_ptr_);
        sq_head_ = reinterpret_cast<unsigned int*>(sq_bytes + params_.sq_off.head);
        sq_tail_ = reinterpret_cast<unsigned int*>(sq_bytes + params_.sq_off.tail);
        sq_ring_mask_ = *reinterpret_cast<unsigned int*>(sq_bytes + params_.sq_off.ring_mask);
        sq_ring_entries_ = *reinterpret_cast<unsigned int*>(sq_bytes + params_.sq_off.ring_entries);
        sq_flags_ = reinterpret_cast<unsigned int*>(sq_bytes + params_.sq_off.flags);
        sq_array_ = reinterpret_cast<unsigned int*>(sq_bytes + params_.sq_off.array);
        sqes_ = static_cast<struct io_uring_sqe*>(sqes_ptr_);

        auto* cq_bytes = static_cast<uint8_t*>(cq_ring_ptr_);
        cq_head_ = reinterpret_cast<unsigned int*>(cq_bytes + params_.cq_off.head);
        cq_tail_ = reinterpret_cast<unsigned int*>(cq_bytes + params_.cq_off.tail);
        cq_ring_mask_ = *reinterpret_cast<unsigned int*>(cq_bytes + params_.cq_off.ring_mask);
        cq_ring_entries_ = *reinterpret_cast<unsigned int*>(cq_bytes + params_.cq_off.ring_entries);
        cq_overflow_ = reinterpret_cast<unsigned int*>(cq_bytes + params_.cq_off.overflow);
        cqes_ = reinterpret_cast<struct io_uring_cqe*>(cq_bytes + params_.cq_off.cqes);

        sqe_tail_ = *sq_tail_;
        sq_committed_tail_ = sqe_tail_;

        return {};
    }

    [[nodiscard]] int fd() const noexcept { return ring_fd_; }
    [[nodiscard]] const struct io_uring_params& params() const noexcept { return params_; }

    // Acquire next available SQE
    struct io_uring_sqe* get_sqe() noexcept {
        unsigned int head = std::atomic_ref(*sq_head_).load(std::memory_order_acquire);
        if (sqe_tail_ - head >= sq_ring_entries_) {
            return nullptr; // Submission queue full
        }
        unsigned int index = sqe_tail_ & sq_ring_mask_;
        struct io_uring_sqe* sqe = &sqes_[index];
        std::memset(sqe, 0, sizeof(*sqe));
        sq_array_[index] = index;
        sqe_tail_++;
        return sqe;
    }

    // Flush prepared SQEs to kernel
    int submit() noexcept {
        return submit_and_wait(0);
    }

    int submit_and_wait(unsigned int min_complete) noexcept {
        unsigned int to_submit = sqe_tail_ - sq_committed_tail_;
        if (to_submit > 0) {
            std::atomic_ref(*sq_tail_).store(sqe_tail_, std::memory_order_release);
            sq_committed_tail_ = sqe_tail_;
        }

        if (to_submit == 0 && min_complete == 0) {
            return 0;
        }

        unsigned int flags = 0;
        if (min_complete > 0) {
            flags |= IORING_ENTER_GETEVENTS;
        }

        int ret = sys_io_uring_enter(ring_fd_, to_submit, min_complete, flags, nullptr);
        return ret;
    }

    // Process all ready CQEs via callback: fn(const io_uring_cqe& cqe)
    template <typename Callback>
    unsigned int for_each_cqe(Callback&& cb) noexcept {
        unsigned int head = std::atomic_ref(*cq_head_).load(std::memory_order_acquire);
        unsigned int tail = std::atomic_ref(*cq_tail_).load(std::memory_order_acquire);
        unsigned int count = 0;

        while (head != tail) {
            struct io_uring_cqe* cqe = &cqes_[head & cq_ring_mask_];
            cb(*cqe);
            head++;
            count++;
        }

        if (count > 0) {
            std::atomic_ref(*cq_head_).store(head, std::memory_order_release);
        }
        return count;
    }

    // Peek if any CQE is available
    struct io_uring_cqe* peek_cqe() noexcept {
        unsigned int head = std::atomic_ref(*cq_head_).load(std::memory_order_acquire);
        unsigned int tail = std::atomic_ref(*cq_tail_).load(std::memory_order_acquire);
        if (head == tail) return nullptr;
        return &cqes_[head & cq_ring_mask_];
    }

    void advance_cq(unsigned int count = 1) noexcept {
        unsigned int head = std::atomic_ref(*cq_head_).load(std::memory_order_relaxed);
        std::atomic_ref(*cq_head_).store(head + count, std::memory_order_release);
    }

    // Helpers to prepare SQEs
    static void prep_accept(struct io_uring_sqe* sqe, int fd, struct sockaddr* addr,
                            socklen_t* addrlen, int flags, uint64_t user_data) noexcept {
        sqe->opcode = IORING_OP_ACCEPT;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<uint64_t>(addr);
        sqe->addr2 = reinterpret_cast<uint64_t>(addrlen);
        sqe->accept_flags = static_cast<__u32>(flags);
        sqe->user_data = user_data;
    }

    static void prep_multishot_accept(struct io_uring_sqe* sqe, int fd, struct sockaddr* addr,
                                      socklen_t* addrlen, int flags, uint64_t user_data) noexcept {
        prep_accept(sqe, fd, addr, addrlen, flags, user_data);
        sqe->ioprio = IORING_ACCEPT_MULTISHOT;
    }

    static void prep_recv(struct io_uring_sqe* sqe, int fd, void* buf, unsigned int len,
                          int flags, uint64_t user_data) noexcept {
        sqe->opcode = IORING_OP_RECV;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<uint64_t>(buf);
        sqe->len = len;
        sqe->msg_flags = static_cast<__u32>(flags);
        sqe->user_data = user_data;
    }

    static void prep_recv_buf_select(struct io_uring_sqe* sqe, int fd, uint16_t bgid,
                                     unsigned int len, int flags, uint64_t user_data) noexcept {
        sqe->opcode = IORING_OP_RECV;
        sqe->flags = IOSQE_BUFFER_SELECT;
        sqe->fd = fd;
        sqe->addr = 0;
        sqe->len = len;
        sqe->msg_flags = static_cast<__u32>(flags);
        sqe->buf_group = bgid;
        sqe->user_data = user_data;
    }

    static void prep_send(struct io_uring_sqe* sqe, int fd, const void* buf, unsigned int len,
                          int flags, uint64_t user_data) noexcept {
        sqe->opcode = IORING_OP_SEND;
        sqe->fd = fd;
        sqe->addr = reinterpret_cast<uint64_t>(buf);
        sqe->len = len;
        sqe->msg_flags = static_cast<__u32>(flags);
        sqe->user_data = user_data;
    }

    static void prep_link_timeout(struct io_uring_sqe* sqe, struct __kernel_timespec* ts,
                                  unsigned int flags, uint64_t user_data) noexcept {
        sqe->opcode = IORING_OP_LINK_TIMEOUT;
        sqe->fd = -1;
        sqe->addr = reinterpret_cast<uint64_t>(ts);
        sqe->len = 1;
        sqe->timeout_flags = flags;
        sqe->user_data = user_data;
    }

    static void prep_splice(struct io_uring_sqe* sqe, int fd_in, int64_t off_in,
                            int fd_out, int64_t off_out, unsigned int nbytes,
                            unsigned int flags, uint64_t user_data) noexcept {
        sqe->opcode = IORING_OP_SPLICE;
        sqe->fd = fd_out;
        sqe->off = static_cast<uint64_t>(off_out);
        sqe->addr = static_cast<uint64_t>(off_in);
        sqe->len = nbytes;
        sqe->splice_flags = flags;
        sqe->splice_fd_in = fd_in;
        sqe->user_data = user_data;
    }

    static void prep_provide_buffers(struct io_uring_sqe* sqe, void* addr, int len,
                                     int nr, uint16_t bgid, int bid, uint64_t user_data) noexcept {
        sqe->opcode = IORING_OP_PROVIDE_BUFFERS;
        sqe->fd = nr;
        sqe->addr = reinterpret_cast<uint64_t>(addr);
        sqe->len = static_cast<__u32>(len);
        sqe->buf_group = bgid;
        sqe->off = static_cast<uint64_t>(bid);
        sqe->user_data = user_data;
    }

    static void prep_close(struct io_uring_sqe* sqe, int fd, uint64_t user_data) noexcept {
        sqe->opcode = IORING_OP_CLOSE;
        sqe->fd = fd;
        sqe->user_data = user_data;
    }

private:
    void destroy() noexcept {
        if (sqes_ptr_ && sqes_ptr_ != MAP_FAILED) {
            ::munmap(sqes_ptr_, sqes_sz_);
            sqes_ptr_ = nullptr;
        }
        if (cq_ring_sz_ > 0 && cq_ring_ptr_ && cq_ring_ptr_ != MAP_FAILED) {
            ::munmap(cq_ring_ptr_, cq_ring_sz_);
            cq_ring_ptr_ = nullptr;
        }
        if (sq_ring_ptr_ && sq_ring_ptr_ != MAP_FAILED) {
            ::munmap(sq_ring_ptr_, sq_ring_sz_);
            sq_ring_ptr_ = nullptr;
        }
        if (ring_fd_ >= 0) {
            ::close(ring_fd_);
            ring_fd_ = -1;
        }
    }

    void move_from(IoRing&& other) noexcept {
        ring_fd_ = other.ring_fd_;
        params_ = other.params_;
        sq_ring_ptr_ = other.sq_ring_ptr_;
        sq_ring_sz_ = other.sq_ring_sz_;
        cq_ring_ptr_ = other.cq_ring_ptr_;
        cq_ring_sz_ = other.cq_ring_sz_;
        sqes_ptr_ = other.sqes_ptr_;
        sqes_sz_ = other.sqes_sz_;

        sq_head_ = other.sq_head_;
        sq_tail_ = other.sq_tail_;
        sq_ring_mask_ = other.sq_ring_mask_;
        sq_ring_entries_ = other.sq_ring_entries_;
        sq_flags_ = other.sq_flags_;
        sq_array_ = other.sq_array_;
        sqes_ = other.sqes_;

        cq_head_ = other.cq_head_;
        cq_tail_ = other.cq_tail_;
        cq_ring_mask_ = other.cq_ring_mask_;
        cq_ring_entries_ = other.cq_ring_entries_;
        cq_overflow_ = other.cq_overflow_;
        cqes_ = other.cqes_;

        sqe_tail_ = other.sqe_tail_;
        sq_committed_tail_ = other.sq_committed_tail_;

        other.ring_fd_ = -1;
        other.sq_ring_ptr_ = nullptr;
        other.cq_ring_ptr_ = nullptr;
        other.sqes_ptr_ = nullptr;
    }

    int ring_fd_{-1};
    struct io_uring_params params_{};

    void* sq_ring_ptr_{nullptr};
    size_t sq_ring_sz_{0};
    void* cq_ring_ptr_{nullptr};
    size_t cq_ring_sz_{0};
    void* sqes_ptr_{nullptr};
    size_t sqes_sz_{0};

    unsigned int* sq_head_{nullptr};
    unsigned int* sq_tail_{nullptr};
    unsigned int sq_ring_mask_{0};
    unsigned int sq_ring_entries_{0};
    unsigned int* sq_flags_{nullptr};
    unsigned int* sq_array_{nullptr};
    struct io_uring_sqe* sqes_{nullptr};

    unsigned int* cq_head_{nullptr};
    unsigned int* cq_tail_{nullptr};
    unsigned int cq_ring_mask_{0};
    unsigned int cq_ring_entries_{0};
    unsigned int* cq_overflow_{nullptr};
    struct io_uring_cqe* cqes_{nullptr};

    unsigned int sqe_tail_{0};
    unsigned int sq_committed_tail_{0};
};

}

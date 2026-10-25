#pragma once

#include <coroutine>
#include <cstdint>
#include <utility>
#include "ring.hpp"

namespace slew {

// Minimal zero-allocation Task for coroutines
class Task {
public:
    struct promise_type {
        Task get_return_object() noexcept {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }

        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };

    explicit Task(std::coroutine_handle<promise_type> h) noexcept : coro_(h) {}
    Task(Task&& other) noexcept : coro_(other.coro_) { other.coro_ = nullptr; }
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (coro_ && coro_.done()) coro_.destroy();
            coro_ = other.coro_;
            other.coro_ = nullptr;
        }
        return *this;
    }
    ~Task() = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

private:
    std::coroutine_handle<promise_type> coro_{nullptr};
};

// Base structure for io_uring awaitables
struct IoAwaitable {
    std::coroutine_handle<> coro{nullptr};
    int32_t res{0};
    uint32_t flags{0};

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void resume_with(int32_t result, uint32_t cqe_flags) noexcept {
        res = result;
        flags = cqe_flags;
        if (coro) {
            coro.resume();
        }
    }
};

// Awaitable for accepting incoming connections
struct AcceptAwaitable : public IoAwaitable {
    IoRing& ring;
    int listen_fd;
    struct sockaddr* addr;
    socklen_t* addrlen;
    int accept_flags;

    AcceptAwaitable(IoRing& r, int fd, struct sockaddr* a, socklen_t* l, int fl = 0) noexcept
        : ring(r), listen_fd(fd), addr(a), addrlen(l), accept_flags(fl) {}

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        coro = h;
        auto* sqe = ring.get_sqe();
        if (!sqe) {
            res = -ENOMEM;
            return false; // Don't suspend, resume immediately with error
        }
        IoRing::prep_accept(sqe, listen_fd, addr, addrlen, accept_flags, reinterpret_cast<uint64_t>(this));
        return true;
    }

    int await_resume() const noexcept {
        return res;
    }
};

// Awaitable for reading using io_uring provided buffer ring (IOSQE_BUFFER_SELECT)
struct RecvBufSelectAwaitable : public IoAwaitable {
    IoRing& ring;
    int fd;
    uint16_t bgid;
    unsigned int len;
    int msg_flags;
    struct __kernel_timespec* timeout_ts;

    RecvBufSelectAwaitable(IoRing& r, int sfd, uint16_t buffer_group,
                           unsigned int max_len, int fl = 0,
                           struct __kernel_timespec* ts = nullptr) noexcept
        : ring(r), fd(sfd), bgid(buffer_group), len(max_len), msg_flags(fl), timeout_ts(ts) {}

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        coro = h;
        auto* sqe = ring.get_sqe();
        if (!sqe) {
            res = -ENOMEM;
            return false;
        }

        IoRing::prep_recv_buf_select(sqe, fd, bgid, len, msg_flags, reinterpret_cast<uint64_t>(this));

        if (timeout_ts) {
            sqe->flags |= IOSQE_IO_LINK;
            auto* t_sqe = ring.get_sqe();
            if (!t_sqe) {
                // If timeout SQE cannot be allocated, proceed without timeout link
                sqe->flags &= ~IOSQE_IO_LINK;
            } else {
                IoRing::prep_link_timeout(t_sqe, timeout_ts, 0, 0); // user_data = 0
            }
        }

        return true;
    }

    struct RecvResult {
        int bytes;
        int buffer_id;
        bool has_buffer;
    };

    RecvResult await_resume() const noexcept {
        RecvResult r{};
        r.bytes = res;
        if (res >= 0 && (flags & IORING_CQE_F_BUFFER)) {
            r.has_buffer = true;
            r.buffer_id = static_cast<int>(flags >> IORING_CQE_BUFFER_SHIFT);
        } else {
            r.has_buffer = false;
            r.buffer_id = -1;
        }
        return r;
    }
};

// Awaitable for sending data to socket
struct SendAwaitable : public IoAwaitable {
    IoRing& ring;
    int fd;
    const void* buf;
    unsigned int len;
    int send_flags;

    SendAwaitable(IoRing& r, int sfd, const void* buffer, unsigned int nbytes, int fl = 0) noexcept
        : ring(r), fd(sfd), buf(buffer), len(nbytes), send_flags(fl) {}

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        coro = h;
        auto* sqe = ring.get_sqe();
        if (!sqe) {
            res = -ENOMEM;
            return false;
        }
        IoRing::prep_send(sqe, fd, buf, len, send_flags, reinterpret_cast<uint64_t>(this));
        return true;
    }

    int await_resume() const noexcept {
        return res;
    }
};

// Awaitable for splicing from file to socket
struct SpliceAwaitable : public IoAwaitable {
    IoRing& ring;
    int fd_in;
    int64_t off_in;
    int fd_out;
    int64_t off_out;
    unsigned int nbytes;
    unsigned int splice_flags;

    SpliceAwaitable(IoRing& r, int in, int64_t off_i, int out, int64_t off_o,
                    unsigned int bytes, unsigned int fl = 0) noexcept
        : ring(r), fd_in(in), off_in(off_i), fd_out(out), off_out(off_o), nbytes(bytes), splice_flags(fl) {}

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        coro = h;
        auto* sqe = ring.get_sqe();
        if (!sqe) {
            res = -ENOMEM;
            return false;
        }
        IoRing::prep_splice(sqe, fd_in, off_in, fd_out, off_out, nbytes, splice_flags, reinterpret_cast<uint64_t>(this));
        return true;
    }

    int await_resume() const noexcept {
        return res;
    }
};

// Awaitable for closing socket
struct CloseAwaitable : public IoAwaitable {
    IoRing& ring;
    int fd;

    CloseAwaitable(IoRing& r, int close_fd) noexcept : ring(r), fd(close_fd) {}

    bool await_suspend(std::coroutine_handle<> h) noexcept {
        coro = h;
        auto* sqe = ring.get_sqe();
        if (!sqe) {
            res = -ENOMEM;
            return false;
        }
        IoRing::prep_close(sqe, fd, reinterpret_cast<uint64_t>(this));
        return true;
    }

    int await_resume() const noexcept {
        return res;
    }
};

}

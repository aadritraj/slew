#pragma once

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <unistd.h>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <iostream>

#include "../core/types.hpp"
#include "../core/ring.hpp"
#include "../core/coro.hpp"
#include "../core/buffer_pool.hpp"
#include "../http/http_types.hpp"
#include "../http/http_parser.hpp"
#include "../http/router.hpp"
#include "../fs/file_cache.hpp"
#include "connection.hpp"

namespace slew {

class Worker {
public:
    Worker(size_t core_id, uint16_t port, std::string docroot,
           const Router& router, int timeout_seconds = 5)
        : core_id_(core_id),
          port_(port),
          docroot_(std::move(docroot)),
          router_(router),
          timeout_sec_(timeout_seconds) {
        timeout_ts_.tv_sec = timeout_sec_;
        timeout_ts_.tv_nsec = 0;
    }

    ~Worker() {
        stop();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void stop() noexcept {
        running_.store(false, std::memory_order_release);
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    void run() {
        // 1. Pin worker thread to target CPU core
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id_, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

        // 2. Initialize raw io_uring ring with cutting-edge Linux 6.0+ flags if supported
        unsigned int ring_flags = 0;
#ifdef IORING_SETUP_COOP_TASKRUN
        ring_flags |= IORING_SETUP_COOP_TASKRUN;
#endif
#ifdef IORING_SETUP_SINGLE_ISSUER
        ring_flags |= IORING_SETUP_SINGLE_ISSUER;
#endif
#ifdef IORING_SETUP_DEFER_TASKRUN
        ring_flags |= IORING_SETUP_DEFER_TASKRUN;
#endif
        auto init_res = ring_.init(4096, ring_flags);
        if (!init_res.ok()) {
#ifdef IORING_SETUP_DEFER_TASKRUN
            init_res = ring_.init(4096, ring_flags & ~IORING_SETUP_DEFER_TASKRUN);
#endif
        }
        if (!init_res.ok()) {
            init_res = ring_.init(4096, 0);
        }
        if (!init_res.ok()) {
            std::cerr << "[Worker " << core_id_ << "] Failed to init io_uring: error "
                      << static_cast<int>(init_res.error()) << std::endl;
            return;
        }

        // 3. Initialize provided buffer pool for zero-copy reads
        buffer_pool_ = std::make_unique<ProvidedBufferPool>();
        auto buf_res = buffer_pool_->init(ring_, 2048, 4096, 1);
        if (!buf_res.ok()) {
            std::cerr << "[Worker " << core_id_ << "] Failed to init buffer pool" << std::endl;
            return;
        }

        // 4. Initialize thread-local file cache and connection pool
        file_cache_ = std::make_unique<FileCache>(docroot_, 2048);
        conn_pool_ = std::make_unique<ConnectionPool>(16384);

        // 5. Setup listening socket with SO_REUSEPORT, TCP_NODELAY and TCP_DEFER_ACCEPT
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) {
            std::cerr << "[Worker " << core_id_ << "] Failed to create socket" << std::endl;
            return;
        }

        int enable = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        ::setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable));
        int defer_sec = timeout_sec_;
        ::setsockopt(listen_fd_, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer_sec, sizeof(defer_sec));

        struct sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_port = htons(port_);
        saddr.sin_addr.s_addr = INADDR_ANY;

        if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&saddr), sizeof(saddr)) < 0) {
            std::cerr << "[Worker " << core_id_ << "] Failed to bind to port " << port_ << std::endl;
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        if (::listen(listen_fd_, 4096) < 0) {
            std::cerr << "[Worker " << core_id_ << "] Failed to listen" << std::endl;
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        running_.store(true, std::memory_order_release);
        coarse_time_ = ::time(nullptr);

        // 6. Launch accept loop coroutine
        accept_loop();

        // 7. Event loop processing io_uring CQEs with batch draining
        while (running_.load(std::memory_order_acquire)) {
            // Refresh coarse timestamp once per event loop round
            coarse_time_ = ::time(nullptr);

            // Opportunistically drain any CQEs already ready before entering kernel
            unsigned int drained = ring_.for_each_cqe([](const struct io_uring_cqe& cqe) {
                if (cqe.user_data != 0) {
                    auto* awaitable = reinterpret_cast<IoAwaitable*>(cqe.user_data);
                    awaitable->resume_with(cqe.res, cqe.flags);
                }
            });

            unsigned int min_complete = (drained > 0) ? 0 : 1;
            int ret = ring_.submit_and_wait(min_complete);
            if (ret < 0 && ret != -EINTR && ret != -EAGAIN) {
                break;
            }

            ring_.for_each_cqe([](const struct io_uring_cqe& cqe) {
                if (cqe.user_data != 0) {
                    auto* awaitable = reinterpret_cast<IoAwaitable*>(cqe.user_data);
                    awaitable->resume_with(cqe.res, cqe.flags);
                }
            });
        }
    }

private:
    Task accept_loop() {
        while (running_.load(std::memory_order_relaxed)) {
            Connection* conn = conn_pool_->acquire();
            if (!conn) {
                // Connection pool saturated, skip iteration
                break;
            }

            int client_fd = co_await AcceptAwaitable{
                ring_,
                listen_fd_,
                reinterpret_cast<struct sockaddr*>(&conn->client_addr),
                &conn->addr_len,
                SOCK_NONBLOCK | SOCK_CLOEXEC
            };

            if (client_fd < 0) {
                conn_pool_->release(conn);
                if (client_fd == -EINTR || client_fd == -EAGAIN) {
                    continue;
                }
                break;
            }

            conn->fd = client_fd;
            handle_client(conn);
        }
    }

    Task handle_client(Connection* conn) {
        while (running_.load(std::memory_order_relaxed)) {
            // Read HTTP request using io_uring provided buffer ring + linked timeout
            auto [bytes, bid, has_buf] = co_await RecvBufSelectAwaitable{
                ring_,
                conn->fd,
                buffer_pool_->group_id(),
                static_cast<unsigned int>(buffer_pool_->buffer_size()),
                0,
                &timeout_ts_
            };

            if (bytes <= 0 || !has_buf) {
                // Client closed connection, error, or timed out (-ETIME)
                break;
            }

            conn->buffer_id = bid;
            const char* raw_req = reinterpret_cast<const char*>(buffer_pool_->get_buffer(bid));

            HttpRequest req{};
            auto parse_res = HttpParser::parse_request(raw_req, static_cast<size_t>(bytes), req);

            HttpResponse resp{};

            if (!parse_res.ok()) {
                if (parse_res.error() == StatusCode::NotSupported) {
                    resp.set_status(HttpStatus::VersionNotSupported);
                    resp.add_header("Content-Type", "text/plain; charset=utf-8");
                    resp.add_header("Content-Length", "28");
                    resp.set_body("HTTP Version Not Supported\r\n");
                } else if (parse_res.error() == StatusCode::LengthRequired) {
                    resp.set_status(HttpStatus::LengthRequired);
                    resp.add_header("Content-Type", "text/plain; charset=utf-8");
                    resp.add_header("Content-Length", "17");
                    resp.set_body("Length Required\r\n");
                } else {
                    resp.set_status(HttpStatus::BadRequest);
                    resp.add_header("Content-Type", "text/plain; charset=utf-8");
                    resp.add_header("Content-Length", "13");
                    resp.set_body("Bad Request\r\n");
                }
                resp.keep_alive = false;

                size_t hlen = resp.serialize_headers(conn->resp_header_buf, sizeof(conn->resp_header_buf), coarse_time_);
                if (hlen + resp.body.size() <= sizeof(conn->resp_header_buf)) {
                    if (!resp.body.empty()) {
                        std::memcpy(conn->resp_header_buf + hlen, resp.body.data(), resp.body.size());
                    }
                    co_await SendAwaitable{ring_, conn->fd, conn->resp_header_buf, static_cast<unsigned int>(hlen + resp.body.size()), 0};
                } else {
                    co_await SendAwaitable{ring_, conn->fd, conn->resp_header_buf, static_cast<unsigned int>(hlen), MSG_MORE};
                    co_await SendAwaitable{ring_, conn->fd, resp.body.data(), static_cast<unsigned int>(resp.body.size()), 0};
                }

                buffer_pool_->return_buffer(ring_, bid);
                conn->buffer_id = -1;
                break;
            }

            // Honor client keep-alive if negotiated
            resp.keep_alive = req.keep_alive;

            // Handle HTTP methods
            if (req.method == HttpMethod::UNKNOWN) {
                resp.set_status(HttpStatus::NotImplemented);
                resp.keep_alive = false;
                resp.add_header("Content-Type", "text/plain; charset=utf-8");
                resp.add_header("Content-Length", "17");
                resp.set_body("Not Implemented\r\n");
            } else {
                dispatch(req, resp);
            }

            // Send response headers & body
            size_t hlen = resp.serialize_headers(conn->resp_header_buf, sizeof(conn->resp_header_buf), coarse_time_);

            if (req.method == HttpMethod::HEAD) {
                // Per RFC 7230, HEAD responses MUST NOT include a message-body
                co_await SendAwaitable{ring_, conn->fd, conn->resp_header_buf, static_cast<unsigned int>(hlen), 0};
            } else if (!resp.body.empty()) {
                if (hlen + resp.body.size() <= sizeof(conn->resp_header_buf)) {
                    // Fast path: pack headers + body into single buffer and send with a single io_uring SQE
                    std::memcpy(conn->resp_header_buf + hlen, resp.body.data(), resp.body.size());
                    co_await SendAwaitable{ring_, conn->fd, conn->resp_header_buf, static_cast<unsigned int>(hlen + resp.body.size()), 0};
                } else {
                    co_await SendAwaitable{ring_, conn->fd, conn->resp_header_buf, static_cast<unsigned int>(hlen), MSG_MORE};
                    co_await SendAwaitable{ring_, conn->fd, resp.body.data(), static_cast<unsigned int>(resp.body.size()), 0};
                }
            } else if (resp.file_fd >= 0) {
                co_await SendAwaitable{ring_, conn->fd, conn->resp_header_buf, static_cast<unsigned int>(hlen), MSG_MORE};
                co_await SpliceAwaitable{
                    ring_,
                    resp.file_fd,
                    resp.file_offset,
                    conn->fd,
                    0,
                    static_cast<unsigned int>(resp.file_size),
                    SPLICE_F_NONBLOCK
                };
            } else {
                co_await SendAwaitable{ring_, conn->fd, conn->resp_header_buf, static_cast<unsigned int>(hlen), 0};
            }

            // Return receive buffer to io_uring pool
            buffer_pool_->return_buffer(ring_, bid);
            conn->buffer_id = -1;

            // Close connection if either side requested close
            if (!resp.keep_alive || !req.keep_alive) {
                break;
            }
        }

        // Teardown connection
        if (conn->buffer_id >= 0) {
            buffer_pool_->return_buffer(ring_, conn->buffer_id);
            conn->buffer_id = -1;
        }

        co_await CloseAwaitable{ring_, conn->fd};
        conn_pool_->release(conn);
    }

    void dispatch(const HttpRequest& req, HttpResponse& resp) {
        // 1. Check dynamic route table first
        const auto* handler = router_.match(req.method, req.path);
        if (handler) {
            (*handler)(req, resp);
            return;
        }

        // 2. Fall back to static file serving (GET / HEAD)
        if (req.method != HttpMethod::GET && req.method != HttpMethod::HEAD) {
            resp.set_status(HttpStatus::MethodNotAllowed);
            resp.add_header("Allow", "GET, HEAD");
            resp.add_header("Content-Type", "text/plain; charset=utf-8");
            resp.add_header("Content-Length", "20");
            resp.set_body("Method Not Allowed\r\n");
            return;
        }

        auto path_res = file_cache_->sanitize_path(req.path);
        if (!path_res.ok()) {
            resp.set_status(HttpStatus::Forbidden);
            resp.add_header("Content-Type", "text/plain; charset=utf-8");
            resp.add_header("Content-Length", "11");
            resp.set_body("Forbidden\r\n");
            return;
        }

        auto file_res = file_cache_->get_or_load(path_res.value());
        if (!file_res.ok()) {
            if (file_res.error() == StatusCode::NotFound) {
                resp.set_status(HttpStatus::NotFound);
                resp.add_header("Content-Type", "text/plain; charset=utf-8");
                resp.add_header("Content-Length", "11");
                resp.set_body("Not Found\r\n");
            } else {
                resp.set_status(HttpStatus::Forbidden);
                resp.add_header("Content-Type", "text/plain; charset=utf-8");
                resp.add_header("Content-Length", "11");
                resp.set_body("Forbidden\r\n");
            }
            return;
        }

        CachedFile* file = file_res.value();
        resp.set_status(HttpStatus::OK);
        resp.add_header("Content-Type", file->mime_type);
        resp.add_header("Last-Modified", file->last_modified);
        resp.add_header("ETag", file->etag);
        resp.add_header("Content-Length", file->size_str);

        if (file->is_in_memory()) {
            resp.set_body(std::string_view(file->content.data(), file->content.size()));
        } else {
            resp.set_file(file->fd, file->size, 0);
        }
    }

    size_t core_id_;
    uint16_t port_;
    std::string docroot_;
    const Router& router_;
    int timeout_sec_;
    struct __kernel_timespec timeout_ts_{};
    time_t coarse_time_{0};

    std::atomic<bool> running_{false};
    int listen_fd_{-1};

    IoRing ring_;
    std::unique_ptr<ProvidedBufferPool> buffer_pool_;
    std::unique_ptr<FileCache> file_cache_;
    std::unique_ptr<ConnectionPool> conn_pool_;
};

}

#include <cassert>
#include <iostream>
#include <unistd.h>
#include "../../include/slew/core/ring.hpp"
#include "../../include/slew/core/buffer_pool.hpp"
#include "../../include/slew/server/connection.hpp"

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

int main() {
    std::cout << "Running IoRing & ConnectionPool unit tests..." << std::endl;

    // Test O(1) LIFO ConnectionPool (independent of io_uring syscall permission)
    {
        slew::ConnectionPool conn_pool(8);
        TEST_ASSERT(conn_pool.capacity() == 8);
        TEST_ASSERT(conn_pool.active_count() == 0);

        slew::Connection* c1 = conn_pool.acquire();
        TEST_ASSERT(c1 != nullptr);
        TEST_ASSERT(c1->in_use);
        TEST_ASSERT(conn_pool.active_count() == 1);

        slew::Connection* c2 = conn_pool.acquire();
        TEST_ASSERT(c2 != nullptr);
        TEST_ASSERT(c2 != c1);
        TEST_ASSERT(conn_pool.active_count() == 2);

        // Release c2 first, then re-acquire: should get c2 back (LIFO cache-locality)
        conn_pool.release(c2);
        TEST_ASSERT(conn_pool.active_count() == 1);

        slew::Connection* c3 = conn_pool.acquire();
        TEST_ASSERT(c3 == c2); // LIFO re-acquired most recent slot!
        TEST_ASSERT(conn_pool.active_count() == 2);

        conn_pool.release(c1);
        conn_pool.release(c3);
        TEST_ASSERT(conn_pool.active_count() == 0);
    }

    slew::IoRing ring;
    auto init_res = ring.init(32);
    if (!init_res.ok()) {
        std::cerr << "IoRing init skipped (requires Linux 5.19+ and bypassing container sandbox): error "
                  << static_cast<int>(init_res.error()) << std::endl;
        std::cout << "ConnectionPool unit tests passed successfully!" << std::endl;
        return 0; // Skip io_uring operations if environment prevents syscall
    }

    TEST_ASSERT(ring.fd() >= 0);

    // Test NOP submission and completion
    auto* sqe = ring.get_sqe();
    TEST_ASSERT(sqe != nullptr);
    sqe->opcode = IORING_OP_NOP;
    sqe->user_data = 0x12345678;

    int submit_res = ring.submit_and_wait(1);
    TEST_ASSERT(submit_res >= 0);

    bool cqe_found = false;
    int cqe_res = -1;
    ring.for_each_cqe([&](const struct io_uring_cqe& cqe) {
        if (cqe.user_data == 0x12345678) {
            cqe_res = cqe.res;
            cqe_found = true;
        }
    });
    TEST_ASSERT(cqe_found);
    TEST_ASSERT(cqe_res == 0);

    // Test Provided Buffer Pool (PBUF_RING or legacy provide_buffers)
    slew::ProvidedBufferPool pool;
    auto pool_res = pool.init(ring, 16, 1024, 1);
    TEST_ASSERT(pool_res.ok());
    TEST_ASSERT(pool.buffer_count() == 16);
    TEST_ASSERT(pool.buffer_size() == 1024);
    TEST_ASSERT(pool.get_buffer(0) != nullptr);

    // Test buffer return
    pool.return_buffer(ring, 0);

    std::cout << "All IoRing & ConnectionPool unit tests passed successfully!" << std::endl;
    return 0;
}

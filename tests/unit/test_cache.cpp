#include <cassert>
#include <iostream>
#include "../../include/slew/fs/file_cache.hpp"

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

int main() {
    std::cout << "Running FileCache unit tests..." << std::endl;

    slew::FileCache cache("./public", 64);

    // Test 1: Directory traversal defense
    {
        auto res1 = cache.sanitize_path("/../../etc/passwd");
        TEST_ASSERT(!res1.ok());
        TEST_ASSERT(res1.error() == slew::StatusCode::AccessDenied);

        auto res2 = cache.sanitize_path("/foo/../bar");
        TEST_ASSERT(!res2.ok());
        TEST_ASSERT(res2.error() == slew::StatusCode::AccessDenied);

        auto res3 = cache.sanitize_path("relative/path");
        TEST_ASSERT(!res3.ok());
    }

    // Test 2: Valid path resolution
    {
        auto res = cache.sanitize_path("/");
        TEST_ASSERT(res.ok());
        TEST_ASSERT(res.value() == "./public/index.html");

        auto res2 = cache.sanitize_path("/index.html");
        TEST_ASSERT(res2.ok());
        TEST_ASSERT(res2.value() == "./public/index.html");
    }

    // Test 3: MIME types
    {
        TEST_ASSERT(slew::FileCache::get_mime_type("index.html") == "text/html; charset=utf-8");
        TEST_ASSERT(slew::FileCache::get_mime_type("style.css") == "text/css; charset=utf-8");
        TEST_ASSERT(slew::FileCache::get_mime_type("app.js") == "application/javascript; charset=utf-8");
        TEST_ASSERT(slew::FileCache::get_mime_type("data.json") == "application/json");
        TEST_ASSERT(slew::FileCache::get_mime_type("image.png") == "image/png");
        TEST_ASSERT(slew::FileCache::get_mime_type("unknown.xyz") == "application/octet-stream");
    }

    // Test 4: File loading and caching
    {
        auto path_res = cache.sanitize_path("/index.html");
        TEST_ASSERT(path_res.ok());

        auto file_res = cache.get_or_load(path_res.value());
        TEST_ASSERT(file_res.ok());
        slew::CachedFile* file = file_res.value();
        TEST_ASSERT(file != nullptr);
        TEST_ASSERT(file->size > 0);
        TEST_ASSERT(file->is_in_memory()); // small file cached in memory
        TEST_ASSERT(!file->content.empty());

        // Repeated access hits cache
        auto file_res2 = cache.get_or_load(path_res.value());
        TEST_ASSERT(file_res2.ok());
        TEST_ASSERT(file_res2.value() == file);
    }

    std::cout << "All FileCache unit tests passed successfully!" << std::endl;
    return 0;
}

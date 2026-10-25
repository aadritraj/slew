#include <cassert>
#include <iostream>
#include <string_view>
#include "../../include/slew/http/http_parser.hpp"

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            std::cerr << "Assertion failed: " #cond " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            return 1; \
        } \
    } while (0)

int main() {
    std::cout << "Running HTTP Parser unit tests..." << std::endl;

    // Test 1: Standard HTTP/1.0 GET request
    {
        std::string_view req_str =
            "GET /index.html HTTP/1.0\r\n"
            "User-Agent: test-client\r\n"
            "Accept: text/html\r\n"
            "\r\n";

        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(res.ok());
        TEST_ASSERT(req.method == slew::HttpMethod::GET);
        TEST_ASSERT(req.path == "/index.html");
        TEST_ASSERT(req.version == slew::HttpVersion::HTTP_1_0);
        TEST_ASSERT(!req.keep_alive); // HTTP/1.0 defaults to close
        TEST_ASSERT(req.get_header("User-Agent") == "test-client");
        TEST_ASSERT(req.get_header("user-agent") == "test-client"); // case-insensitive
        TEST_ASSERT(req.get_header("Accept") == "text/html");
        TEST_ASSERT(req.get_header("Non-Existent").empty());
    }

    // Test 2: HTTP/1.0 Keep-Alive request
    {
        std::string_view req_str =
            "GET /api/test?foo=bar HTTP/1.0\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";

        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(res.ok());
        TEST_ASSERT(req.path == "/api/test");
        TEST_ASSERT(req.query == "foo=bar");
        TEST_ASSERT(req.keep_alive);
    }

    // Test 3: RFC 1945 Simple-Request (no version header)
    {
        std::string_view req_str = "GET /simple\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(res.ok());
        TEST_ASSERT(req.method == slew::HttpMethod::GET);
        TEST_ASSERT(req.path == "/simple");
        TEST_ASSERT(!req.keep_alive);
    }

    // Test 4: POST request with body
    {
        std::string_view req_str =
            "POST /submit HTTP/1.0\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "{\"hello\":123}";

        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(res.ok());
        TEST_ASSERT(req.method == slew::HttpMethod::POST);
        TEST_ASSERT(req.content_length == 13);
        TEST_ASSERT(req.body == "{\"hello\":123}");
    }

    // Test 5: Malformed request - missing space
    {
        std::string_view req_str = "GET/index.html HTTP/1.0\r\n\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(!res.ok());
    }

    // Test 6: Malformed request - bad version
    {
        std::string_view req_str = "GET /index.html HTTP/9.9\r\n\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(!res.ok());
        TEST_ASSERT(res.error() == slew::StatusCode::NotSupported);
    }

    // Test 7: Malformed header - missing colon
    {
        std::string_view req_str =
            "GET / HTTP/1.0\r\n"
            "BrokenHeaderNoColon\r\n"
            "\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(!res.ok());
    }

    // Test 8: HTTP/1.1 GET with Host header (persistent by default)
    {
        std::string_view req_str =
            "GET /index.html HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "User-Agent: curl/8.0\r\n"
            "\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(res.ok());
        TEST_ASSERT(req.method == slew::HttpMethod::GET);
        TEST_ASSERT(req.version == slew::HttpVersion::HTTP_1_1);
        TEST_ASSERT(req.path == "/index.html");
        TEST_ASSERT(req.host == "example.com");
        TEST_ASSERT(req.keep_alive); // HTTP/1.1 defaults to keep-alive
    }

    // Test 9: HTTP/1.1 GET with Connection: close
    {
        std::string_view req_str =
            "GET /status HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "Connection: close\r\n"
            "\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(res.ok());
        TEST_ASSERT(!req.keep_alive);
    }

    // Test 10: HTTP/1.1 missing Host header (RFC 7230 violation)
    {
        std::string_view req_str =
            "GET /index.html HTTP/1.1\r\n"
            "User-Agent: curl/8.0\r\n"
            "\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(!res.ok());
        TEST_ASSERT(res.error() == slew::StatusCode::MissingHost);
    }

    // Test 11: HTTP/1.1 duplicate Host headers (RFC 7230 violation)
    {
        std::string_view req_str =
            "GET /index.html HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "Host: rogue.com\r\n"
            "\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(!res.ok());
        TEST_ASSERT(res.error() == slew::StatusCode::MissingHost);
    }

    // Test 12: HTTP/1.1 empty Host header (RFC 7230 violation)
    {
        std::string_view req_str =
            "GET /index.html HTTP/1.1\r\n"
            "Host:   \r\n"
            "\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(!res.ok());
        TEST_ASSERT(res.error() == slew::StatusCode::MissingHost);
    }

    // Test 13: HTTP/1.1 POST without Content-Length (411 Length Required)
    {
        std::string_view req_str =
            "POST /api/upload HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "\r\n";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(!res.ok());
        TEST_ASSERT(res.error() == slew::StatusCode::LengthRequired);
    }

    // Test 14: HTTP/1.1 POST with Content-Length
    {
        std::string_view req_str =
            "POST /api/upload HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        slew::HttpRequest req{};
        auto res = slew::HttpParser::parse_request(req_str.data(), req_str.size(), req);
        TEST_ASSERT(res.ok());
        TEST_ASSERT(req.method == slew::HttpMethod::POST);
        TEST_ASSERT(req.content_length == 5);
        TEST_ASSERT(req.body == "hello");
    }

    // Test 15: HTTP/1.1 methods PUT, DELETE, OPTIONS, PATCH
    {
        std::string_view methods[] = {
            "PUT /item HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n",
            "DELETE /item HTTP/1.1\r\nHost: a\r\n\r\n",
            "OPTIONS * HTTP/1.1\r\nHost: a\r\n\r\n",
            "PATCH /item HTTP/1.1\r\nHost: a\r\nContent-Length: 0\r\n\r\n"
        };
        slew::HttpMethod expected[] = {
            slew::HttpMethod::PUT,
            slew::HttpMethod::DELETE,
            slew::HttpMethod::OPTIONS,
            slew::HttpMethod::PATCH
        };

        for (size_t i = 0; i < 4; ++i) {
            slew::HttpRequest req{};
            auto res = slew::HttpParser::parse_request(methods[i].data(), methods[i].size(), req);
            TEST_ASSERT(res.ok());
            TEST_ASSERT(req.method == expected[i]);
        }
    }

    // Test 16: HttpResponse auto Content-Length calculation
    {
        slew::HttpResponse resp{};
        resp.set_status(slew::HttpStatus::OK);
        resp.add_header("Content-Type", "application/json");
        resp.set_body("{\"status\":\"ok\",\"server\":\"slew/1.1\"}");

        char buf[512] = {};
        size_t len = resp.serialize_headers(buf, sizeof(buf), 0);
        TEST_ASSERT(len > 0);
        std::string_view serialized(buf, len);
        TEST_ASSERT(serialized.find("Content-Length: 35\r\n") != std::string_view::npos);
        TEST_ASSERT(serialized.find("Content-Type: application/json\r\n") != std::string_view::npos);
    }

    std::cout << "All HTTP Parser unit tests passed successfully!" << std::endl;
    return 0;
}

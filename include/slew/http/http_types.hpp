#pragma once

#include <string_view>
#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include "../core/types.hpp"

namespace slew {

enum class HttpMethod : uint8_t {
    UNKNOWN = 0,
    GET = 1,
    HEAD = 2,
    POST = 3,
    PUT = 4,
    DELETE = 5,
    OPTIONS = 6,
    PATCH = 7,
};

constexpr std::string_view method_string(HttpMethod m) noexcept {
    switch (m) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE:  return "DELETE";
        case HttpMethod::OPTIONS: return "OPTIONS";
        case HttpMethod::PATCH:   return "PATCH";
        default:                  return "UNKNOWN";
    }
}

enum class HttpVersion : uint8_t {
    UNKNOWN = 0,
    HTTP_1_0 = 1,
    HTTP_1_1 = 2,
};

constexpr std::string_view version_string(HttpVersion v) noexcept {
    switch (v) {
        case HttpVersion::HTTP_1_0: return "HTTP/1.0";
        case HttpVersion::HTTP_1_1: return "HTTP/1.1";
        default:                    return "UNKNOWN";
    }
}

inline std::string_view get_rfc1123_date_header(time_t current_time = 0) noexcept {
    static thread_local time_t s_last_time = 0;
    static thread_local char s_buf[48] = {};
    static thread_local size_t s_len = 0;
    time_t now = (current_time != 0) ? current_time : ::time(nullptr);
    if (now != s_last_time) [[unlikely]] {
        s_last_time = now;
        struct tm tm_buf;
        gmtime_r(&now, &tm_buf);
        static constexpr const char* DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static constexpr const char* MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

        char* p = s_buf;
        std::memcpy(p, "Date: ", 6); p += 6;
        int wday = (tm_buf.tm_wday >= 0 && tm_buf.tm_wday < 7) ? tm_buf.tm_wday : 0;
        std::memcpy(p, DAYS[wday], 3); p += 3;
        *p++ = ','; *p++ = ' ';
        *p++ = static_cast<char>('0' + (tm_buf.tm_mday / 10));
        *p++ = static_cast<char>('0' + (tm_buf.tm_mday % 10));
        *p++ = ' ';
        int mon = (tm_buf.tm_mon >= 0 && tm_buf.tm_mon < 12) ? tm_buf.tm_mon : 0;
        std::memcpy(p, MONTHS[mon], 3); p += 3;
        *p++ = ' ';
        int year = 1900 + tm_buf.tm_year;
        *p++ = static_cast<char>('0' + ((year / 1000) % 10));
        *p++ = static_cast<char>('0' + ((year / 100) % 10));
        *p++ = static_cast<char>('0' + ((year / 10) % 10));
        *p++ = static_cast<char>('0' + (year % 10));
        *p++ = ' ';
        *p++ = static_cast<char>('0' + (tm_buf.tm_hour / 10));
        *p++ = static_cast<char>('0' + (tm_buf.tm_hour % 10));
        *p++ = ':';
        *p++ = static_cast<char>('0' + (tm_buf.tm_min / 10));
        *p++ = static_cast<char>('0' + (tm_buf.tm_min % 10));
        *p++ = ':';
        *p++ = static_cast<char>('0' + (tm_buf.tm_sec / 10));
        *p++ = static_cast<char>('0' + (tm_buf.tm_sec % 10));
        std::memcpy(p, " GMT\r\n", 6); p += 6;
        s_len = static_cast<size_t>(p - s_buf);
    }
    return std::string_view(s_buf, s_len);
}

struct HttpHeader {
    std::string_view name;
    std::string_view value;
};

struct HttpRequest {
    static constexpr size_t MAX_HEADERS = 32;

    HttpMethod method{HttpMethod::UNKNOWN};
    HttpVersion version{HttpVersion::UNKNOWN};
    std::string_view raw_uri{};
    std::string_view path{};
    std::string_view query{};
    std::array<HttpHeader, MAX_HEADERS> headers{};
    size_t header_count{0};
    std::string_view body{};
    size_t content_length{0};
    bool has_content_length{false};
    bool keep_alive{false};
    size_t host_count{0};
    std::string_view host{};

    [[nodiscard]] std::string_view get_header(std::string_view name) const noexcept {
        for (size_t i = 0; i < header_count; ++i) {
            if (iequals(headers[i].name, name)) {
                return headers[i].value;
            }
        }
        return {};
    }

    [[nodiscard]] bool has_header(std::string_view name) const noexcept {
        return !get_header(name).empty();
    }
};

struct HttpResponse {
    static constexpr size_t MAX_HEADERS = 16;

    HttpStatus status{HttpStatus::OK};
    std::array<HttpHeader, MAX_HEADERS> headers{};
    size_t header_count{0};
    std::string_view body{};
    int file_fd{-1};
    int64_t file_offset{0};
    size_t file_size{0};
    bool keep_alive{false};

    void set_status(HttpStatus s) noexcept {
        status = s;
    }

    bool add_header(std::string_view name, std::string_view value) noexcept {
        if (header_count >= MAX_HEADERS) return false;
        headers[header_count++] = {name, value};
        return true;
    }

    void set_body(std::string_view b) noexcept {
        body = b;
    }

    void set_file(int fd, size_t size, int64_t offset = 0) noexcept {
        file_fd = fd;
        file_size = size;
        file_offset = offset;
    }

    // Serializes response headers into preallocated buffer.
    // Returns number of bytes written, or 0 on overflow.
    size_t serialize_headers(char* dest, size_t max_len, time_t current_time = 0) const noexcept {
        std::string_view sline = status_line(status);
        if (sline.size() > max_len) return 0;

        char* p = dest;
        char* end = dest + max_len;

        std::memcpy(p, sline.data(), sline.size());
        p += sline.size();

        // Server header
        std::string_view server_hdr = "Server: slew/1.1\r\n";
        if (static_cast<size_t>(end - p) < server_hdr.size()) return 0;
        std::memcpy(p, server_hdr.data(), server_hdr.size());
        p += server_hdr.size();

        // Date header
        std::string_view date_hdr = get_rfc1123_date_header(current_time);
        if (static_cast<size_t>(end - p) < date_hdr.size()) return 0;
        std::memcpy(p, date_hdr.data(), date_hdr.size());
        p += date_hdr.size();

        // Connection header
        std::string_view conn_hdr = keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        if (static_cast<size_t>(end - p) < conn_hdr.size()) return 0;
        std::memcpy(p, conn_hdr.data(), conn_hdr.size());
        p += conn_hdr.size();

        // Custom headers
        bool has_clen = false;
        for (size_t i = 0; i < header_count; ++i) {
            const auto& h = headers[i];
            if (iequals(h.name, "Content-Length")) {
                has_clen = true;
            }
            size_t needed = h.name.size() + 2 + h.value.size() + 2; // "Name: Value\r\n"
            if (static_cast<size_t>(end - p) < needed) return 0;

            std::memcpy(p, h.name.data(), h.name.size());
            p += h.name.size();
            *p++ = ':';
            *p++ = ' ';
            std::memcpy(p, h.value.data(), h.value.size());
            p += h.value.size();
            *p++ = '\r';
            *p++ = '\n';
        }

        // Auto-add Content-Length for in-memory body if omitted (branchless fast_itoa)
        if (!has_clen && file_fd < 0 && status != HttpStatus::NoContent && status != HttpStatus::NotModified) {
            static constexpr std::string_view CLEN_PREFIX = "Content-Length: ";
            if (static_cast<size_t>(end - p) < CLEN_PREFIX.size() + 24 + 2) return 0;
            std::memcpy(p, CLEN_PREFIX.data(), CLEN_PREFIX.size());
            p += CLEN_PREFIX.size();
            p = write_u64_digits(p, body.size());
            *p++ = '\r';
            *p++ = '\n';
        }

        // Final CRLF
        if (static_cast<size_t>(end - p) < 2) return 0;
        *p++ = '\r';
        *p++ = '\n';

        return static_cast<size_t>(p - dest);
    }
};

}

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <concepts>
#include <utility>

namespace slew {

enum class StatusCode : int {
    Ok = 0,
    InvalidParam = 1,
    OutOfMemory = 2,
    SystemError = 3,
    ParserError = 4,
    Timeout = 5,
    ConnectionClosed = 6,
    NotFound = 7,
    AccessDenied = 8,
    BufferFull = 9,
    NotSupported = 10,
    MissingHost = 11,
    LengthRequired = 12,
};

template <typename T, typename E = StatusCode>
class Result {
public:
    constexpr Result(const T& val) : has_val_(true), val_(val) {}
    constexpr Result(T&& val) : has_val_(true), val_(std::move(val)) {}
    constexpr Result(E err) : has_val_(false), err_(err) {}

    ~Result() {
        if (has_val_) {
            val_.~T();
        }
    }

    constexpr Result(const Result& other) : has_val_(other.has_val_) {
        if (has_val_) {
            new (&val_) T(other.val_);
        } else {
            err_ = other.err_;
        }
    }

    constexpr Result(Result&& other) noexcept : has_val_(other.has_val_) {
        if (has_val_) {
            new (&val_) T(std::move(other.val_));
        } else {
            err_ = other.err_;
        }
    }

    Result& operator=(const Result& other) {
        if (this != &other) {
            if (has_val_) val_.~T();
            has_val_ = other.has_val_;
            if (has_val_) new (&val_) T(other.val_);
            else err_ = other.err_;
        }
        return *this;
    }

    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            if (has_val_) val_.~T();
            has_val_ = other.has_val_;
            if (has_val_) new (&val_) T(std::move(other.val_));
            else err_ = other.err_;
        }
        return *this;
    }

    constexpr bool ok() const noexcept { return has_val_; }
    constexpr explicit operator bool() const noexcept { return has_val_; }

    constexpr const T& value() const& noexcept { return val_; }
    constexpr T& value() & noexcept { return val_; }
    constexpr T&& value() && noexcept { return std::move(val_); }

    constexpr const T* operator->() const noexcept { return &val_; }
    constexpr T* operator->() noexcept { return &val_; }
    constexpr const T& operator*() const& noexcept { return val_; }
    constexpr T& operator*() & noexcept { return val_; }

    constexpr E error() const noexcept { return err_; }

private:
    bool has_val_;
    union {
        T val_;
        E err_;
    };
};

// Specialization for void
template <typename E>
class Result<void, E> {
public:
    constexpr Result() : has_val_(true), err_() {}
    constexpr Result(E err) : has_val_(false), err_(err) {}

    constexpr bool ok() const noexcept { return has_val_; }
    constexpr explicit operator bool() const noexcept { return has_val_; }
    constexpr E error() const noexcept { return err_; }

private:
    bool has_val_;
    E err_;
};

// Standard HTTP Status Codes per RFC 1945 & RFC 7230
enum class HttpStatus : uint16_t {
    OK = 200,
    Created = 201,
    Accepted = 202,
    NoContent = 204,
    MovedPermanently = 301,
    MovedTemporarily = 302,
    NotModified = 304,
    BadRequest = 400,
    Unauthorized = 401,
    Forbidden = 403,
    NotFound = 404,
    MethodNotAllowed = 405,
    RequestTimeout = 408,
    LengthRequired = 411,
    InternalServerError = 500,
    NotImplemented = 501,
    BadGateway = 502,
    ServiceUnavailable = 503,
    VersionNotSupported = 505
};

constexpr std::string_view status_phrase(HttpStatus status) noexcept {
    switch (status) {
        case HttpStatus::OK:                  return "OK";
        case HttpStatus::Created:             return "Created";
        case HttpStatus::Accepted:            return "Accepted";
        case HttpStatus::NoContent:           return "No Content";
        case HttpStatus::MovedPermanently:    return "Moved Permanently";
        case HttpStatus::MovedTemporarily:    return "Moved Temporarily";
        case HttpStatus::NotModified:         return "Not Modified";
        case HttpStatus::BadRequest:          return "Bad Request";
        case HttpStatus::Unauthorized:        return "Unauthorized";
        case HttpStatus::Forbidden:           return "Forbidden";
        case HttpStatus::NotFound:            return "Not Found";
        case HttpStatus::MethodNotAllowed:    return "Method Not Allowed";
        case HttpStatus::RequestTimeout:      return "Request Timeout";
        case HttpStatus::LengthRequired:      return "Length Required";
        case HttpStatus::InternalServerError: return "Internal Server Error";
        case HttpStatus::NotImplemented:      return "Not Implemented";
        case HttpStatus::BadGateway:          return "Bad Gateway";
        case HttpStatus::ServiceUnavailable:  return "Service Unavailable";
        case HttpStatus::VersionNotSupported: return "HTTP Version Not Supported";
        default:                              return "Unknown";
    }
}

constexpr std::string_view status_line(HttpStatus status) noexcept {
    switch (status) {
        case HttpStatus::OK:                  return "HTTP/1.1 200 OK\r\n";
        case HttpStatus::Created:             return "HTTP/1.1 201 Created\r\n";
        case HttpStatus::Accepted:            return "HTTP/1.1 202 Accepted\r\n";
        case HttpStatus::NoContent:           return "HTTP/1.1 204 No Content\r\n";
        case HttpStatus::MovedPermanently:    return "HTTP/1.1 301 Moved Permanently\r\n";
        case HttpStatus::MovedTemporarily:    return "HTTP/1.1 302 Moved Temporarily\r\n";
        case HttpStatus::NotModified:         return "HTTP/1.1 304 Not Modified\r\n";
        case HttpStatus::BadRequest:          return "HTTP/1.1 400 Bad Request\r\n";
        case HttpStatus::Unauthorized:        return "HTTP/1.1 401 Unauthorized\r\n";
        case HttpStatus::Forbidden:           return "HTTP/1.1 403 Forbidden\r\n";
        case HttpStatus::NotFound:            return "HTTP/1.1 404 Not Found\r\n";
        case HttpStatus::MethodNotAllowed:    return "HTTP/1.1 405 Method Not Allowed\r\n";
        case HttpStatus::RequestTimeout:      return "HTTP/1.1 408 Request Timeout\r\n";
        case HttpStatus::LengthRequired:      return "HTTP/1.1 411 Length Required\r\n";
        case HttpStatus::InternalServerError: return "HTTP/1.1 500 Internal Server Error\r\n";
        case HttpStatus::NotImplemented:      return "HTTP/1.1 501 Not Implemented\r\n";
        case HttpStatus::BadGateway:          return "HTTP/1.1 502 Bad Gateway\r\n";
        case HttpStatus::ServiceUnavailable:  return "HTTP/1.1 503 Service Unavailable\r\n";
        case HttpStatus::VersionNotSupported: return "HTTP/1.1 505 HTTP Version Not Supported\r\n";
        default:                              return "HTTP/1.1 500 Internal Server Error\r\n";
    }
}

// Utility string functions
constexpr char ascii_tolower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_tolower(a[i]) != ascii_tolower(b[i])) return false;
    }
    return true;
}

constexpr std::string_view trim_whitespace(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

// Fast branchless 2-digit lookup table for integer-to-string formatting (Stepanov / Mathisen)
inline char* write_u64_digits(char* p, uint64_t val) noexcept {
    static constexpr char DIGIT_PAIRS[200] = {
        '0','0','0','1','0','2','0','3','0','4','0','5','0','6','0','7','0','8','0','9',
        '1','0','1','1','1','2','1','3','1','4','1','5','1','6','1','7','1','8','1','9',
        '2','0','2','1','2','2','2','3','2','4','2','5','2','6','2','7','2','8','2','9',
        '3','0','3','1','3','2','3','3','3','4','3','5','3','6','3','7','3','8','3','9',
        '4','0','4','1','4','2','4','3','4','4','4','5','4','6','4','7','4','8','4','9',
        '5','0','5','1','5','2','5','3','5','4','5','5','5','6','5','7','5','8','5','9',
        '6','0','6','1','6','2','6','3','6','4','6','5','6','6','6','7','6','8','6','9',
        '7','0','7','1','7','2','7','3','7','4','7','5','7','6','7','7','7','8','7','9',
        '8','0','8','1','8','2','8','3','8','4','8','5','8','6','8','7','8','8','8','9',
        '9','0','9','1','9','2','9','3','9','4','9','5','9','6','9','7','9','8','9','9'
    };
    char temp[24];
    char* cur = temp + 23;
    while (val >= 100) {
        uint64_t q = val / 100;
        uint32_t r = static_cast<uint32_t>(val - (q * 100));
        val = q;
        cur -= 2;
        cur[0] = DIGIT_PAIRS[r * 2];
        cur[1] = DIGIT_PAIRS[r * 2 + 1];
    }
    if (val < 10) {
        *--cur = static_cast<char>('0' + val);
    } else {
        cur -= 2;
        cur[0] = DIGIT_PAIRS[val * 2];
        cur[1] = DIGIT_PAIRS[val * 2 + 1];
    }
    size_t len = static_cast<size_t>((temp + 23) - cur);
    std::memcpy(p, cur, len);
    return p + len;
}

inline bool parse_size(std::string_view s, size_t& out) noexcept {
    s = trim_whitespace(s);
    if (s.empty()) return false;
    size_t result = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        size_t digit = static_cast<size_t>(c - '0');
        // Overflow check
        if (result > (static_cast<size_t>(-1) - digit) / 10) return false;
        result = result * 10 + digit;
    }
    out = result;
    return true;
}

}

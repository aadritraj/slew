#pragma once

#include <string_view>
#include <cstring>
#include <cstdint>
#include "http_types.hpp"
#include "../core/types.hpp"

namespace slew {

class HttpParser {
public:
    static bool header_contains_token(std::string_view header_val, std::string_view token) noexcept {
        size_t start = 0;
        while (start < header_val.size()) {
            size_t comma = header_val.find(',', start);
            if (comma == std::string_view::npos) comma = header_val.size();
            std::string_view item = trim_whitespace(header_val.substr(start, comma - start));
            if (iequals(item, token)) return true;
            start = comma + 1;
        }
        return false;
    }

    // Fast search for CRLF ("\r\n") using 64-bit SWAR scanning
    static const char* find_crlf(const char* start, const char* end) noexcept {
        const char* p = start;

        // Align pointer to 8-byte boundary
        while (p + 1 < end && (reinterpret_cast<uintptr_t>(p) & 7) != 0) {
            if (p[0] == '\r' && p[1] == '\n') return p;
            p++;
        }

        // 8-byte block scanning for '\r' (0x0D)
        static constexpr uint64_t MASK_0D = 0x0D0D0D0D0D0D0D0DULL;
        static constexpr uint64_t HIGH_BITS = 0x8080808080808080ULL;

        while (p + 8 <= end) {
            uint64_t v;
            std::memcpy(&v, p, sizeof(v));

            // Has zero byte test on (v ^ MASK_0D)
            uint64_t diff = v ^ MASK_0D;
            uint64_t has_cr = (diff - 0x0101010101010101ULL) & ~diff & HIGH_BITS;

            if (has_cr != 0) {
                // At least one '\r' in this 8-byte word, find exactly where
                for (size_t i = 0; i < 8; ++i) {
                    if (p + i + 1 < end && p[i] == '\r' && p[i + 1] == '\n') {
                        return p + i;
                    }
                }
            }
            p += 8;
        }

        // Tail bytes
        while (p + 1 < end) {
            if (p[0] == '\r' && p[1] == '\n') return p;
            p++;
        }

        return nullptr;
    }

    // Parses HTTP/1.0 request from buffer.
    // Returns number of bytes consumed by request line and headers on success.
    static Result<size_t> parse_request(const char* buf, size_t len, HttpRequest& req) noexcept {
        if (!buf || len == 0) {
            return StatusCode::ParserError;
        }

        const char* p = buf;
        const char* end = buf + len;

        // 1. Parse Request-Line: Method SP Request-URI SP HTTP-Version CRLF
        const char* line_end = find_crlf(p, end);
        if (!line_end) {
            return StatusCode::ParserError; // Incomplete or malformed request line
        }

        const char* line_start = p;
        p = line_end + 2; // advance past CRLF

        // Fast-path integer method detection
        const char* uri_start = nullptr;
        size_t rem = static_cast<size_t>(line_end - line_start);

        if (rem >= 4) {
            uint32_t w4;
            std::memcpy(&w4, line_start, 4);
            if (w4 == 0x20544547) { // "GET " (little-endian)
                req.method = HttpMethod::GET;
                uri_start = line_start + 4;
            } else if (w4 == 0x20545550) { // "PUT "
                req.method = HttpMethod::PUT;
                uri_start = line_start + 4;
            } else if (rem >= 5) {
                if (w4 == 0x54534F50 && line_start[4] == ' ') { // "POST "
                    req.method = HttpMethod::POST;
                    uri_start = line_start + 5;
                } else if (w4 == 0x44414548 && line_start[4] == ' ') { // "HEAD "
                    req.method = HttpMethod::HEAD;
                    uri_start = line_start + 5;
                }
            }
        }

        if (!uri_start) {
            // General method extraction fallback
            const char* sp1 = static_cast<const char*>(std::memchr(line_start, ' ', line_end - line_start));
            if (!sp1 || sp1 == line_start) {
                return StatusCode::ParserError;
            }

            std::string_view method_str(line_start, static_cast<size_t>(sp1 - line_start));
            if (method_str.empty()) {
                return StatusCode::ParserError;
            }

            // Validate method token (must not contain '/' or delimiters)
            for (char c : method_str) {
                if (c <= 32 || c >= 127 || c == '/' || c == '?' || c == '#' || c == '[' || c == ']') {
                    return StatusCode::ParserError;
                }
            }

            if (method_str == "GET") {
                req.method = HttpMethod::GET;
            } else if (method_str == "HEAD") {
                req.method = HttpMethod::HEAD;
            } else if (method_str == "POST") {
                req.method = HttpMethod::POST;
            } else if (method_str == "PUT") {
                req.method = HttpMethod::PUT;
            } else if (method_str == "DELETE") {
                req.method = HttpMethod::DELETE;
            } else if (method_str == "OPTIONS") {
                req.method = HttpMethod::OPTIONS;
            } else if (method_str == "PATCH") {
                req.method = HttpMethod::PATCH;
            } else {
                req.method = HttpMethod::UNKNOWN;
            }

            uri_start = sp1 + 1;
        }
        while (uri_start < line_end && *uri_start == ' ') {
            uri_start++;
        }
        if (uri_start >= line_end) {
            return StatusCode::ParserError;
        }

        // Extract URI
        const char* sp2 = static_cast<const char*>(std::memchr(uri_start, ' ', line_end - uri_start));
        if (!sp2) {
            // Simple-Request in RFC 1945: GET /path (no version), defaults to HTTP/0.9 or HTTP/1.0
            req.raw_uri = std::string_view(uri_start, static_cast<size_t>(line_end - uri_start));
            req.version = HttpVersion::HTTP_1_0;
            req.keep_alive = false;
        } else {
            req.raw_uri = std::string_view(uri_start, static_cast<size_t>(sp2 - uri_start));

            // Extract HTTP Version
            const char* ver_start = sp2 + 1;
            while (ver_start < line_end && *ver_start == ' ') {
                ver_start++;
            }
            size_t ver_len = static_cast<size_t>(line_end - ver_start);
            if (ver_len == 8) [[likely]] {
                uint64_t v;
                std::memcpy(&v, ver_start, 8);
                if (v == 0x312E312F50545448ULL) { // "HTTP/1.1"
                    req.version = HttpVersion::HTTP_1_1;
                    req.keep_alive = true;  // In HTTP/1.1, keep-alive is default per RFC 7230
                } else if (v == 0x302E312F50545448ULL) { // "HTTP/1.0"
                    req.version = HttpVersion::HTTP_1_0;
                    req.keep_alive = false; // In HTTP/1.0, close is default per RFC 1945
                } else {
                    req.version = HttpVersion::UNKNOWN;
                    return StatusCode::NotSupported;
                }
            } else {
                req.version = HttpVersion::UNKNOWN;
                return StatusCode::NotSupported;
            }
        }

        // Split URI into path and query
        size_t qpos = req.raw_uri.find('?');
        if (qpos != std::string_view::npos) {
            req.path = req.raw_uri.substr(0, qpos);
            req.query = req.raw_uri.substr(qpos + 1);
        } else {
            req.path = req.raw_uri;
            req.query = {};
        }

        // 2. Parse Headers
        req.header_count = 0;
        req.content_length = 0;
        req.has_content_length = false;
        req.host_count = 0;
        req.host = {};

        while (p < end) {
            const char* next_crlf = find_crlf(p, end);
            if (!next_crlf) {
                return StatusCode::ParserError; // Truncated header
            }

            // Empty line indicates end of headers
            if (next_crlf == p) {
                p += 2; // Skip final CRLF
                break;
            }

            const char* colon = static_cast<const char*>(std::memchr(p, ':', next_crlf - p));
            if (!colon || colon == p) {
                return StatusCode::ParserError; // Malformed header: no colon
            }

            std::string_view hname(p, static_cast<size_t>(colon - p));
            std::string_view hval(colon + 1, static_cast<size_t>(next_crlf - (colon + 1)));

            hname = trim_whitespace(hname);
            hval = trim_whitespace(hval);

            if (req.header_count < HttpRequest::MAX_HEADERS) {
                req.headers[req.header_count++] = {hname, hval};
            }

            // Inspect critical headers inline with fast length pre-filtering
            if (hname.size() == 4 && iequals(hname, "Host")) {
                req.host_count++;
                req.host = hval;
            } else if (hname.size() == 10 && iequals(hname, "Connection")) {
                if (header_contains_token(hval, "close")) {
                    req.keep_alive = false;
                } else if (header_contains_token(hval, "keep-alive")) {
                    req.keep_alive = true;
                }
            } else if (hname.size() == 14 && iequals(hname, "Content-Length")) {
                req.has_content_length = true;
                if (!parse_size(hval, req.content_length)) {
                    return StatusCode::ParserError;
                }
            }

            p = next_crlf + 2;
        }

        // 3. Post-header RFC validations
        if (req.version == HttpVersion::HTTP_1_1) {
            // Exactly one non-empty Host header is mandatory in HTTP/1.1
            if (req.host_count != 1 || req.host.empty()) {
                return StatusCode::MissingHost;
            }
            // Host header must not contain invalid characters or spaces
            for (char c : req.host) {
                if (c <= 32 || c >= 127) {
                    return StatusCode::MissingHost;
                }
            }

            // Methods with body must have Content-Length in HTTP/1.1
            if ((req.method == HttpMethod::POST || req.method == HttpMethod::PUT || req.method == HttpMethod::PATCH) && !req.has_content_length) {
                return StatusCode::LengthRequired;
            }
        }

        size_t consumed = static_cast<size_t>(p - buf);

        // If body data is present in buffer
        if (consumed < len && req.content_length > 0) {
            size_t available_body = len - consumed;
            size_t body_len = (available_body < req.content_length) ? available_body : req.content_length;
            req.body = std::string_view(buf + consumed, body_len);
        } else {
            req.body = {};
        }

        return consumed;
    }
};

}

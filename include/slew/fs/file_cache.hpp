#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <unordered_map>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctime>
#include <cstdio>
#include "../core/types.hpp"

namespace slew {

struct CachedFile {
    std::string path;
    int fd{-1};
    size_t size{0};
    char size_str[32]{};
    time_t mtime{0};
    char last_modified[32]{};
    char etag[32]{};
    std::string_view mime_type{"application/octet-stream"};
    std::vector<char> content; // populated if size <= SMALL_FILE_THRESHOLD
    uint64_t last_access{0};

    [[nodiscard]] bool is_in_memory() const noexcept {
        return !content.empty();
    }
};

class FileCache {
public:
    static constexpr size_t DEFAULT_CAPACITY = 1024;
    static constexpr size_t SMALL_FILE_THRESHOLD = 64 * 1024; // 64 KB

    explicit FileCache(std::string docroot = "./public", size_t capacity = DEFAULT_CAPACITY)
        : docroot_(std::move(docroot)), capacity_(capacity) {
        // Strip trailing slash if present
        while (!docroot_.empty() && docroot_.back() == '/') {
            docroot_.pop_back();
        }
    }

    ~FileCache() {
        clear();
    }

    FileCache(const FileCache&) = delete;
    FileCache& operator=(const FileCache&) = delete;

    FileCache(FileCache&& other) noexcept
        : docroot_(std::move(other.docroot_)),
          capacity_(other.capacity_),
          access_counter_(other.access_counter_),
          cache_(std::move(other.cache_)) {}

    FileCache& operator=(FileCache&& other) noexcept {
        if (this != &other) {
            clear();
            docroot_ = std::move(other.docroot_);
            capacity_ = other.capacity_;
            access_counter_ = other.access_counter_;
            cache_ = std::move(other.cache_);
        }
        return *this;
    }

    void clear() noexcept {
        for (auto& [_, file] : cache_) {
            if (file.fd >= 0) {
                ::close(file.fd);
                file.fd = -1;
            }
        }
        cache_.clear();
    }

    static std::string_view get_mime_type(std::string_view path) noexcept {
        size_t dot = path.rfind('.');
        if (dot == std::string_view::npos) {
            return "application/octet-stream";
        }
        std::string_view ext = path.substr(dot);
        // huh?
        if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
        if (ext == ".css")  return "text/css; charset=utf-8";
        if (ext == ".js")   return "application/javascript; charset=utf-8";
        if (ext == ".json") return "application/json";
        if (ext == ".txt")  return "text/plain; charset=utf-8";
        if (ext == ".png")  return "image/png";
        if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
        if (ext == ".gif")  return "image/gif";
        if (ext == ".svg")  return "image/svg+xml";
        if (ext == ".ico")  return "image/x-icon";
        if (ext == ".wasm") return "application/wasm";
        if (ext == ".pdf")  return "application/pdf";
        if (ext == ".xml")  return "application/xml";
        if (ext == ".mp4")  return "video/mp4";
        return "application/octet-stream";
    }

    // Resolves and normalizes path; guards against directory traversal attacks
    Result<std::string> sanitize_path(std::string_view uri_path) const noexcept {
        if (uri_path.empty() || uri_path[0] != '/') {
            return StatusCode::AccessDenied;
        }

        // Check for directory traversal "../" or "\0"
        if (uri_path.find('\0') != std::string_view::npos) {
            return StatusCode::AccessDenied;
        }

        std::string clean;
        clean.reserve(docroot_.size() + uri_path.size() + 16);
        clean.append(docroot_);

        if (uri_path == "/" || uri_path.empty()) {
            clean.append("/index.html");
            return clean;
        }

        // Tokenize and verify no ".." components
        size_t i = 0;
        while (i < uri_path.size()) {
            while (i < uri_path.size() && uri_path[i] == '/') i++;
            if (i >= uri_path.size()) break;

            size_t start = i;
            while (i < uri_path.size() && uri_path[i] != '/') i++;
            std::string_view segment = uri_path.substr(start, i - start);

            if (segment == ".") {
                continue;
            }
            if (segment == "..") {
                return StatusCode::AccessDenied; // Directory traversal forbidden!
            }

            clean.push_back('/');
            clean.append(segment);
        }

        // If path already ends in slash, target index.html directly
        if (uri_path.back() == '/') {
            clean.append("/index.html");
            return clean;
        }

        // Fast path: if clean is already in cache, skip filesystem stat() entirely!
        if (cache_.find(clean) != cache_.end()) {
            return clean;
        }

        // If not in cache, check if it's a directory (only when no extension is present or directory exists)
        size_t last_slash = clean.rfind('/');
        size_t last_dot = clean.rfind('.');
        if (last_dot == std::string_view::npos || (last_slash != std::string_view::npos && last_dot < last_slash)) {
            struct stat st;
            if (::stat(clean.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                clean.append("/index.html");
            }
        }

        return clean;
    }

    // Lookup or load file into thread-local cache
    Result<CachedFile*> get_or_load(const std::string& full_path) noexcept {
        access_counter_++;

        auto it = cache_.find(full_path);
        if (it != cache_.end()) {
            it->second.last_access = access_counter_;
            return &it->second;
        }

        // File not in cache: stat and load
        struct stat st;
        if (::stat(full_path.c_str(), &st) != 0) {
            if (errno == ENOENT) return StatusCode::NotFound;
            if (errno == EACCES) return StatusCode::AccessDenied;
            return StatusCode::SystemError;
        }

        if (!S_ISREG(st.st_mode)) {
            return StatusCode::NotFound;
        }

        // Check cache capacity, evict LRU if needed
        if (cache_.size() >= capacity_) {
            evict_lru();
        }

        CachedFile file{};
        file.path = full_path;
        file.size = static_cast<size_t>(st.st_size);
        char* p_end = write_u64_digits(file.size_str, file.size);
        *p_end = '\0';
        file.mtime = st.st_mtime;
        file.mime_type = get_mime_type(full_path);
        file.last_access = access_counter_;

        // Format RFC 1123 date: e.g. "Sun, 06 Nov 1994 08:49:37 GMT"
        struct tm tm_buf;
        gmtime_r(&file.mtime, &tm_buf);
        std::strftime(file.last_modified, sizeof(file.last_modified),
                      "%a, %d %b %Y %H:%M:%S GMT", &tm_buf);

        // Compute simple ETag: "\"mtime_hex-size_hex\""
        std::snprintf(file.etag, sizeof(file.etag), "\"%lx-%lx\"",
                      static_cast<unsigned long>(file.mtime),
                      static_cast<unsigned long>(file.size));

        int fd = ::open(full_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return StatusCode::AccessDenied;
        }

        if (file.size <= SMALL_FILE_THRESHOLD) {
            // In-memory cache for small files
            file.content.resize(file.size);
            if (file.size > 0) {
                ssize_t n = ::read(fd, file.content.data(), file.size);
                if (n < 0 || static_cast<size_t>(n) != file.size) {
                    ::close(fd);
                    return StatusCode::SystemError;
                }
            }
            ::close(fd);
            file.fd = -1; // Not needed open since content is in RAM
        } else {
            // Keep descriptor open for zero-copy splice
            file.fd = fd;
        }

        auto [ins_it, _] = cache_.emplace(full_path, std::move(file));
        return &ins_it->second;
    }

private:
    void evict_lru() noexcept {
        if (cache_.empty()) return;
        auto oldest_it = cache_.begin();
        uint64_t min_access = oldest_it->second.last_access;

        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->second.last_access < min_access) {
                min_access = it->second.last_access;
                oldest_it = it;
            }
        }

        if (oldest_it->second.fd >= 0) {
            ::close(oldest_it->second.fd);
        }
        cache_.erase(oldest_it);
    }

    std::string docroot_;
    size_t capacity_{DEFAULT_CAPACITY};
    uint64_t access_counter_{0};
    std::unordered_map<std::string, CachedFile> cache_;
};

}

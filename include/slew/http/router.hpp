#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include "http_types.hpp"
#include "../core/types.hpp"

namespace slew {

using RouteHandler = std::function<void(const HttpRequest&, HttpResponse&)>;

struct Route {
    HttpMethod method;
    std::string path;
    bool is_prefix{false};
    RouteHandler handler;
};

class Router {
public:
    Router() = default;

    void add_route(HttpMethod method, std::string path, RouteHandler handler) {
        routes_.push_back(Route{
            .method = method,
            .path = std::move(path),
            .is_prefix = false,
            .handler = std::move(handler)
        });
    }

    void add_prefix_route(HttpMethod method, std::string prefix, RouteHandler handler) {
        routes_.push_back(Route{
            .method = method,
            .path = std::move(prefix),
            .is_prefix = true,
            .handler = std::move(handler)
        });
    }

    // Convenience helpers
    void get(std::string path, RouteHandler handler) {
        add_route(HttpMethod::GET, std::move(path), std::move(handler));
    }

    void post(std::string path, RouteHandler handler) {
        add_route(HttpMethod::POST, std::move(path), std::move(handler));
    }

    void head(std::string path, RouteHandler handler) {
        add_route(HttpMethod::HEAD, std::move(path), std::move(handler));
    }

    void put(std::string path, RouteHandler handler) {
        add_route(HttpMethod::PUT, std::move(path), std::move(handler));
    }

    void del(std::string path, RouteHandler handler) {
        add_route(HttpMethod::DELETE, std::move(path), std::move(handler));
    }

    void options(std::string path, RouteHandler handler) {
        add_route(HttpMethod::OPTIONS, std::move(path), std::move(handler));
    }

    void patch(std::string path, RouteHandler handler) {
        add_route(HttpMethod::PATCH, std::move(path), std::move(handler));
    }

    // Match request against registered routes
    [[nodiscard]] const RouteHandler* match(HttpMethod method, std::string_view path) const noexcept {
        for (const auto& r : routes_) {
            if (r.method == method || r.method == HttpMethod::UNKNOWN) {
                if (r.is_prefix) {
                    if (path.size() >= r.path.size() && path.starts_with(r.path)) {
                        return &r.handler;
                    }
                } else {
                    if (path.size() == r.path.size() && path == r.path) {
                        return &r.handler;
                    }
                }
            }
        }
        return nullptr;
    }

    [[nodiscard]] size_t route_count() const noexcept {
        return routes_.size();
    }

private:
    std::vector<Route> routes_;
};

}

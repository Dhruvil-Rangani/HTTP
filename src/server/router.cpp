#include "server/router.hpp"

#include <exception>
#include <string_view>

namespace httpd {

void Router::add(std::string method, std::string path, Handler handler) {
  routes_.push_back({std::move(method), std::move(path), false, std::move(handler)});
}

void Router::add_prefix(std::string method, std::string prefix, Handler handler) {
  routes_.push_back({std::move(method), std::move(prefix), true, std::move(handler)});
}

void Router::dispatch(const Request& req, Response& res) const {
  std::string_view method = req.method == "HEAD" ? std::string_view("GET") : std::string_view(req.method);
  std::string allow;
  for (const Route& r : routes_) {
    bool match = r.prefix ? req.path.compare(0, r.path.size(), r.path) == 0 : req.path == r.path;
    if (!match) continue;
    if (r.method == method) {
      try {
        r.handler(req, res);
      } catch (const std::exception& e) {
        res = Response::text(500, std::string("internal error: ") + e.what() + "\n");
      }
      return;
    }
    if (allow.find(r.method) == std::string::npos) {
      if (!allow.empty()) allow.append(", ");
      allow.append(r.method);
      if (r.method == "GET") allow.append(", HEAD");
    }
  }
  if (!allow.empty()) {
    res = Response::text(405, "method not allowed\n");
    res.headers.set("Allow", allow);
  } else {
    res = Response::text(404, "not found\n");
  }
}

}  // namespace httpd

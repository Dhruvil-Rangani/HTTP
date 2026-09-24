#pragma once

#include <functional>
#include <string>
#include <vector>

#include "http/request_parser.hpp"
#include "http/response.hpp"

namespace httpd {

// Handlers run on the event-loop thread that owns the connection: they must
// not block (no sleeping, no synchronous network calls).
using Handler = std::function<void(const Request&, Response&)>;

class Router {
 public:
  // Exact path match.
  void add(std::string method, std::string path, Handler handler);
  // Matches any path that starts with `prefix`.
  void add_prefix(std::string method, std::string prefix, Handler handler);

  // Routes in registration order. HEAD is served by the GET handler (the body
  // is dropped at write time). Unknown path -> 404, known path with another
  // method -> 405 + Allow. Handler exceptions become a 500.
  void dispatch(const Request& req, Response& res) const;

 private:
  struct Route {
    std::string method;
    std::string path;
    bool prefix;
    Handler handler;
  };
  std::vector<Route> routes_;
};

}  // namespace httpd

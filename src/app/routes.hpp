#pragma once

#include <string>

#include "server/server.hpp"

namespace httpd {

struct AppOptions {
  std::string static_root = "public";
};

// Registers the demo/ops endpoints (see README for the list).
void install_routes(Server& server, const AppOptions& opts);

}  // namespace httpd

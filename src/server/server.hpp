#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "http/request_parser.hpp"
#include "server/router.hpp"

namespace httpd {

struct ServerConfig {
  std::string bind_address = "0.0.0.0";
  std::uint16_t port = 8080;       // 0 = let the kernel pick (see Server::port())
  unsigned threads = 0;            // 0 = one event loop per online CPU
  bool pin_threads = false;        // pin worker i to the i-th allowed CPU
  int backlog = 4096;
  std::chrono::milliseconds idle_timeout{30'000};    // keep-alive idle limit
  std::chrono::milliseconds header_timeout{10'000};  // slowloris guard
  std::chrono::milliseconds shutdown_grace{5'000};   // drain window on stop()
  ParserLimits limits;
};

// Multi-reactor HTTP/1.1 server for Linux.
//
// Each worker thread owns an SO_REUSEPORT listening socket, an epoll instance
// and every connection it accepts ("shared-nothing", one event loop per core):
// the kernel spreads incoming connections across the per-thread accept queues,
// so the hot path takes no locks and a connection never migrates between
// threads (good cache locality).
class Server {
 public:
  explicit Server(ServerConfig cfg);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Register routes before start().
  Router& router() noexcept { return router_; }
  const ServerConfig& config() const noexcept { return cfg_; }

  // Binds all listening sockets and spawns the workers. Throws std::system_error.
  void start();
  // Graceful shutdown: stop accepting, finish in-flight responses, close idle
  // keep-alive connections, force-close whatever remains after shutdown_grace.
  // Thread-safe; may be called from any thread.
  void stop();
  // Blocks until all workers have exited.
  void wait();

  std::uint16_t port() const noexcept { return port_; }
  unsigned worker_count() const noexcept { return static_cast<unsigned>(workers_.size()); }
  std::string metrics_text() const;

 private:
  class Worker;
  friend class Worker;

  ServerConfig cfg_;
  Router router_;
  std::uint16_t port_ = 0;
  std::chrono::steady_clock::time_point started_at_;
  std::vector<std::unique_ptr<Worker>> workers_;
};

}  // namespace httpd

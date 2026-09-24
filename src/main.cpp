#include <pthread.h>
#include <signal.h>

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

#include "app/routes.hpp"
#include "server/server.hpp"

namespace {

void usage(const char* argv0) {
  std::printf(
      "usage: %s [options]\n"
      "  --port N               listen port (default 8080, 0 = ephemeral)\n"
      "  --bind ADDR            IPv4 bind address (default 0.0.0.0)\n"
      "  --threads N            event-loop workers (default: online CPUs)\n"
      "  --pin                  pin each worker to its own CPU\n"
      "  --root DIR             directory served under /static/ (default ./public)\n"
      "  --idle-timeout-ms N    keep-alive idle timeout (default 30000)\n"
      "  --header-timeout-ms N  max time to receive request headers (default 10000)\n"
      "  --max-body BYTES       request body limit (default 8388608)\n",
      argv0);
}

unsigned long parse_number(const char* flag, const char* value) {
  if (value == nullptr) {
    std::fprintf(stderr, "httpd: %s needs a value\n", flag);
    std::exit(2);
  }
  char* end = nullptr;
  errno = 0;
  unsigned long v = std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') {
    std::fprintf(stderr, "httpd: invalid number for %s: %s\n", flag, value);
    std::exit(2);
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  httpd::ServerConfig cfg;
  httpd::AppOptions app;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
    if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      return 0;
    } else if (arg == "--port") {
      unsigned long port = parse_number("--port", next);
      if (port > 65535) {
        std::fprintf(stderr, "httpd: port out of range\n");
        return 2;
      }
      cfg.port = static_cast<std::uint16_t>(port);
      ++i;
    } else if (arg == "--bind" && next) {
      cfg.bind_address = next;
      ++i;
    } else if (arg == "--threads") {
      cfg.threads = static_cast<unsigned>(parse_number("--threads", next));
      ++i;
    } else if (arg == "--pin") {
      cfg.pin_threads = true;
    } else if (arg == "--root" && next) {
      app.static_root = next;
      ++i;
    } else if (arg == "--idle-timeout-ms") {
      cfg.idle_timeout = std::chrono::milliseconds(parse_number("--idle-timeout-ms", next));
      ++i;
    } else if (arg == "--header-timeout-ms") {
      cfg.header_timeout = std::chrono::milliseconds(parse_number("--header-timeout-ms", next));
      ++i;
    } else if (arg == "--max-body") {
      cfg.limits.max_body_bytes = parse_number("--max-body", next);
      ++i;
    } else {
      std::fprintf(stderr, "httpd: unknown or incomplete option '%s'\n", argv[i]);
      usage(argv[0]);
      return 2;
    }
  }

  // Block SIGINT/SIGTERM before any thread exists so every worker inherits the
  // mask; the main thread then receives them synchronously with sigwait() and
  // can run ordinary (non async-signal-safe) shutdown code.
  sigset_t signals;
  sigemptyset(&signals);
  sigaddset(&signals, SIGINT);
  sigaddset(&signals, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &signals, nullptr);

  httpd::Server server(cfg);
  httpd::install_routes(server, app);
  try {
    server.start();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "httpd: failed to start: %s\n", e.what());
    return 1;
  }
  std::printf("httpd listening on %s:%u with %u worker(s)%s\n", cfg.bind_address.c_str(), server.port(),
              server.worker_count(), cfg.pin_threads ? " (pinned)" : "");
  std::fflush(stdout);

  int sig = 0;
  sigwait(&signals, &sig);
  std::printf("httpd: received %s, draining connections...\n", strsignal(sig));
  std::fflush(stdout);
  server.stop();
  server.wait();
  std::printf("httpd: stopped\n");
  return 0;
}

#include "app/routes.hpp"

#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <memory>

#include "app/static_files.hpp"
#include "util/crc32.hpp"

namespace httpd {
namespace {

constexpr std::uint64_t kMaxGeneratedBytes = 1ull << 30;  // 1 GiB
constexpr std::uint64_t kMaxStreamChunks = 100'000;
constexpr std::size_t kBytesPiece = 64 * 1024;

bool parse_u64(std::string_view s, std::uint64_t& out) {
  if (s.empty()) return false;
  auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
  return ec == std::errc() && ptr == s.data() + s.size();
}

// 64 KiB of a..z plus one extra alphabet, so any window starting at
// offset % 26 can be sliced out without wrapping.
const std::string& pattern() {
  static const std::string p = [] {
    std::string s(kBytesPiece + 26, '\0');
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>('a' + i % 26);
    return s;
  }();
  return p;
}

std::string json_escape(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') out.push_back('\\');
    if (static_cast<unsigned char>(c) >= 0x20) out.push_back(c);
  }
  return out;
}

const char* kIndex =
    "httpd-cpp: an epoll-based HTTP/1.1 server in C++17\n"
    "\n"
    "  GET  /healthz         liveness probe\n"
    "  GET  /metrics         Prometheus metrics (per-worker counters, latency histogram)\n"
    "  GET  /debug/info      build, kernel and runtime information (JSON)\n"
    "  POST /echo            echoes the request body (Content-Length or chunked)\n"
    "  GET  /stream/{n}      n chunks via chunked encoding + CRC-32 trailer\n"
    "  GET  /bytes/{n}       n bytes streamed with backpressure (throughput tests)\n"
    "  GET  /static/{path}   files from --root, zero-copy with sendfile(2)\n";

}  // namespace

void install_routes(Server& server, const AppOptions& opts) {
  Router& r = server.router();

  r.add("GET", "/", [](const Request&, Response& res) { res = Response::text(200, kIndex); });

  r.add("GET", "/healthz", [](const Request&, Response& res) { res = Response::text(200, "ok\n"); });

  r.add("GET", "/metrics", [&server](const Request&, Response& res) {
    res = Response::text(200, server.metrics_text(), "text/plain; version=0.0.4; charset=utf-8");
  });

  r.add("GET", "/debug/info", [&server, root = opts.static_root](const Request&, Response& res) {
    utsname u{};
    ::uname(&u);
    const auto& cfg = server.config();
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "{\"pid\":%d,\"workers\":%u,\"pinned\":%s,\"port\":%u,"
                  "\"online_cpus\":%ld,\"page_size\":%ld,"
                  "\"kernel\":\"%s %s %s\",\"compiler\":\"%s\",\"cplusplus\":%ld,"
                  "\"static_root\":\"%s\","
                  "\"limits\":{\"max_request_line\":%zu,\"max_header_bytes\":%zu,\"max_body_bytes\":%zu}}\n",
                  static_cast<int>(::getpid()), server.worker_count(), cfg.pin_threads ? "true" : "false",
                  server.port(), ::sysconf(_SC_NPROCESSORS_ONLN), ::sysconf(_SC_PAGESIZE),
                  json_escape(u.sysname).c_str(), json_escape(u.release).c_str(), json_escape(u.machine).c_str(),
                  json_escape(__VERSION__).c_str(), static_cast<long>(__cplusplus), json_escape(root).c_str(),
                  cfg.limits.max_request_line, cfg.limits.max_header_bytes, cfg.limits.max_body_bytes);
    res = Response::text(200, buf, "application/json");
  });

  r.add("POST", "/echo", [](const Request& req, Response& res) {
    auto type = req.headers.get("Content-Type");
    res = Response::text(200, req.body, type ? *type : std::string_view("application/octet-stream"));
  });

  // Chunked transfer coding with trailers: the checksum of the body can only
  // be known after the last byte, which is exactly what trailers are for.
  r.add_prefix("GET", "/stream/", [](const Request& req, Response& res) {
    std::uint64_t n = 0;
    if (!parse_u64(std::string_view(req.path).substr(8), n) || n > kMaxStreamChunks) {
      res = Response::text(400, "usage: /stream/{0.." + std::to_string(kMaxStreamChunks) + "}\n");
      return;
    }
    res.headers.set("Content-Type", "text/plain; charset=utf-8");
    res.headers.set("Trailer", "X-Chunk-Count, X-Content-CRC32");
    res.producer = [i = std::uint64_t{0}, n, crc = std::uint32_t{0}](std::string& out, Headers& trailers) mutable {
      if (i < n) {
        std::string line = "chunk " + std::to_string(i) + " of " + std::to_string(n) + "\n";
        crc = crc32(line, crc);
        out += line;
        ++i;
      }
      if (i < n) return true;
      char hex[9];
      std::snprintf(hex, sizeof hex, "%08x", crc);
      trailers.set("X-Chunk-Count", std::to_string(n));
      trailers.set("X-Content-CRC32", hex);
      return false;
    };
  });

  // Large bodies generated on the fly: the producer is only asked for more when
  // the socket has drained, so a slow client cannot make the server buffer 1 GiB.
  r.add_prefix("GET", "/bytes/", [](const Request& req, Response& res) {
    std::uint64_t n = 0;
    if (!parse_u64(std::string_view(req.path).substr(7), n) || n > kMaxGeneratedBytes) {
      res = Response::text(400, "usage: /bytes/{0.." + std::to_string(kMaxGeneratedBytes) + "}\n");
      return;
    }
    res.headers.set("Content-Type", "application/octet-stream");
    res.stream_length = n;
    res.producer = [sent = std::uint64_t{0}, n](std::string& out, Headers&) mutable {
      const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(n - sent, kBytesPiece));
      out.append(pattern(), static_cast<std::size_t>(sent % 26), take);
      sent += take;
      return sent < n;
    };
  });

  auto files = std::make_shared<StaticFiles>(opts.static_root);
  if (!files->available()) {
    std::fprintf(stderr, "httpd: warning: static root '%s' not found; /static/ will return 404\n",
                 opts.static_root.c_str());
  }
  r.add_prefix("GET", "/static/", [files](const Request& req, Response& res) {
    files->serve(std::string_view(req.path).substr(8), res);
  });
}

}  // namespace httpd

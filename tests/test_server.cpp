// End-to-end tests: a real Server on an ephemeral port, driven over loopback
// TCP sockets with raw bytes, so framing, keep-alive, pipelining, timeouts and
// shutdown are exercised exactly as a client would see them.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/routes.hpp"
#include "server/server.hpp"
#include "testing.hpp"
#include "util/crc32.hpp"
#include "util/unique_fd.hpp"

using namespace httpd;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
  fs::path path;
  TempDir() {
    char tmpl[] = "/tmp/httpd-test-XXXXXX";
    const char* p = ::mkdtemp(tmpl);
    REQUIRE(p != nullptr);
    path = p;
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  void write(const std::string& rel, const std::string& content) const {
    fs::create_directories((path / rel).parent_path());
    std::ofstream(path / rel, std::ios::binary) << content;
  }
};

struct HttpResponse {
  int status = 0;
  Headers headers;
  std::string body;
  Headers trailers;
};

class Client {
 public:
  explicit Client(std::uint16_t port) : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    REQUIRE(fd_.valid());
    timeval tv{5, 0};  // never hang the test suite
    ::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    ::setsockopt(fd_.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    REQUIRE(::connect(fd_.get(), reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0);
  }

  bool send(std::string_view s) {
    while (!s.empty()) {
      ssize_t n = ::send(fd_.get(), s.data(), s.size(), MSG_NOSIGNAL);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      s.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
  }

  // Reads one response using whichever framing the server chose.
  std::optional<HttpResponse> read_response(bool head_request = false) {
    HttpResponse r;
    std::string line;
    if (!read_line(line) || line.compare(0, 9, "HTTP/1.1 ") != 0 || line.size() < 12) return std::nullopt;
    r.status = std::stoi(line.substr(9, 3));
    for (;;) {
      if (!read_line(line)) return std::nullopt;
      if (line.empty()) break;
      std::size_t colon = line.find(':');
      if (colon == std::string::npos) return std::nullopt;
      r.headers.add(line.substr(0, colon), trim_ows(std::string_view(line).substr(colon + 1)));
    }
    if (head_request || r.status / 100 == 1 || r.status == 204 || r.status == 304) return r;

    if (r.headers.has_token("Transfer-Encoding", "chunked")) {
      for (;;) {
        if (!read_line(line)) return std::nullopt;
        std::size_t size = std::stoul(line, nullptr, 16);
        if (size == 0) break;
        std::string crlf;
        if (!read_exact(size, r.body) || !read_exact(2, crlf) || crlf != "\r\n") return std::nullopt;
      }
      for (;;) {
        if (!read_line(line)) return std::nullopt;
        if (line.empty()) break;
        std::size_t colon = line.find(':');
        r.trailers.add(line.substr(0, colon), trim_ows(std::string_view(line).substr(colon + 1)));
      }
    } else if (auto cl = r.headers.get("Content-Length")) {
      if (!read_exact(std::stoul(std::string(*cl)), r.body)) return std::nullopt;
    } else {
      while (fill()) {
      }
      r.body = std::move(buf_);
      buf_.clear();
    }
    return r;
  }

  // True once the server has closed the connection (EOF or RST).
  bool closed_by_peer() {
    if (!buf_.empty()) return false;
    char c;
    for (;;) {
      ssize_t n = ::recv(fd_.get(), &c, 1, 0);
      if (n == 0) return true;
      if (n > 0) {
        buf_.push_back(c);
        return false;
      }
      if (errno == EINTR) continue;
      return errno == ECONNRESET;
    }
  }

 private:
  bool fill() {
    char tmp[64 * 1024];
    for (;;) {
      ssize_t n = ::recv(fd_.get(), tmp, sizeof tmp, 0);
      if (n > 0) {
        buf_.append(tmp, static_cast<std::size_t>(n));
        return true;
      }
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
  }
  bool read_line(std::string& line) {
    std::size_t p;
    while ((p = buf_.find("\r\n")) == std::string::npos) {
      if (!fill()) return false;
    }
    line = buf_.substr(0, p);
    buf_.erase(0, p + 2);
    return true;
  }
  bool read_exact(std::size_t n, std::string& out) {
    while (buf_.size() < n) {
      if (!fill()) return false;
    }
    out.append(buf_, 0, n);
    buf_.erase(0, n);
    return true;
  }

  UniqueFd fd_;
  std::string buf_;
};

struct TestServer {
  explicit TestServer(ServerConfig cfg = {}) : server(localize(std::move(cfg))) {
    root.write("hello.txt", "hello from disk\n");
    root.write("index.html", "<h1>index</h1>\n");
    root.write("sub/index.html", "<h1>sub</h1>\n");
    fs::create_symlink("/etc/hostname", root.path / "escape.txt");
    AppOptions app;
    app.static_root = root.path.string();
    install_routes(server, app);
    server.start();
  }
  ~TestServer() {
    server.stop();
    server.wait();
  }
  static ServerConfig localize(ServerConfig c) {
    c.bind_address = "127.0.0.1";
    c.port = 0;
    if (c.threads == 0) c.threads = 2;
    return c;
  }
  std::uint16_t port() const { return server.port(); }

  TempDir root;
  Server server;
};

std::string get(const std::string& path, const std::string& extra = "") {
  return "GET " + path + " HTTP/1.1\r\nHost: test\r\n" + extra + "\r\n";
}

double metric(const std::string& text, const std::string& series) {
  std::size_t pos = text.find("\n" + series + " ");
  if (pos == std::string::npos) return -1;
  return std::strtod(text.c_str() + pos + series.size() + 2, nullptr);
}

}  // namespace

TEST(serves_healthz) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send(get("/healthz")));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 200);
  CHECK_EQ(r->body, "ok\n");
  CHECK(r->headers.has("Date"));
  CHECK(!r->headers.has("Connection"));  // HTTP/1.1 is persistent by default
}

TEST(keep_alive_reuses_one_connection) {
  TestServer s;
  Client c(s.port());
  for (int i = 0; i < 5; ++i) {
    REQUIRE(c.send(get("/healthz")));
    auto r = c.read_response();
    REQUIRE(r);
    CHECK_EQ(r->status, 200);
  }
}

TEST(pipelined_requests_are_answered_in_order) {
  TestServer s;
  Client c(s.port());
  // One write, five requests - including a streamed body in the middle, which
  // must hold back the requests behind it until it has been fully sent.
  REQUIRE(c.send(get("/bytes/1") + get("/stream/3") + get("/bytes/2") +
                 "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\npong" + get("/bytes/3")));
  auto r1 = c.read_response();
  auto r2 = c.read_response();
  auto r3 = c.read_response();
  auto r4 = c.read_response();
  auto r5 = c.read_response();
  REQUIRE(r1 && r2 && r3 && r4 && r5);
  CHECK_EQ(r1->body, "a");
  CHECK_EQ(r2->body, "chunk 0 of 3\nchunk 1 of 3\nchunk 2 of 3\n");
  CHECK_EQ(r3->body, "ab");
  CHECK_EQ(r4->body, "pong");
  CHECK_EQ(r5->body, "abc");
}

TEST(malformed_request_gets_400_then_close) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send("GARBAGE\r\n\r\n"));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 400);
  CHECK_EQ(r->headers.get("Connection").value_or(""), "close");
  CHECK(c.closed_by_peer());
}

TEST(smuggling_attempt_is_rejected) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "0\r\n\r\nGET /healthz HTTP/1.1\r\nHost: t\r\n\r\n"));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 400);
  CHECK(c.closed_by_peer());  // the smuggled second request is never executed
}

TEST(not_found_and_method_not_allowed) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send(get("/nope")));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 404);
  REQUIRE(c.send("DELETE /healthz HTTP/1.1\r\nHost: t\r\n\r\n"));
  r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 405);
  CHECK_EQ(r->headers.get("Allow").value_or(""), "GET, HEAD");
}

TEST(echoes_chunked_request_body) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send("POST /echo HTTP/1.1\r\nHost: t\r\nContent-Type: text/plain\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n"));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 200);
  CHECK_EQ(r->body, "hello world");
  CHECK_EQ(r->headers.get("Content-Type").value_or(""), "text/plain");
}

TEST(expect_100_continue_handshake) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send("POST /echo HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: 5\r\n\r\n"));
  auto interim = c.read_response();
  REQUIRE(interim);
  CHECK_EQ(interim->status, 100);
  REQUIRE(c.send("hello"));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 200);
  CHECK_EQ(r->body, "hello");
}

TEST(streams_chunked_body_with_crc_trailer) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send(get("/stream/50")));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->headers.get("Transfer-Encoding").value_or(""), "chunked");
  CHECK_EQ(r->headers.get("Trailer").value_or(""), "X-Chunk-Count, X-Content-CRC32");
  CHECK(r->body.rfind("chunk 49 of 50\n") == r->body.size() - 15);
  char expected[9];
  std::snprintf(expected, sizeof expected, "%08x", crc32(r->body));
  CHECK_EQ(r->trailers.get("X-Content-CRC32").value_or(""), expected);
  CHECK_EQ(r->trailers.get("X-Chunk-Count").value_or(""), "50");
}

TEST(large_body_streams_under_backpressure) {
  TestServer s;
  Client c(s.port());
  const std::size_t n = 8 * 1024 * 1024;
  REQUIRE(c.send(get("/bytes/" + std::to_string(n))));
  // Don't read yet: the kernel buffers fill up and the server has to park the
  // producer until EPOLLOUT says the socket drained.
  std::this_thread::sleep_for(200ms);
  auto r = c.read_response();
  REQUIRE(r);
  REQUIRE(r->body.size() == n);
  bool pattern_ok = true;
  for (std::size_t i = 0; i < n; i += 4093) pattern_ok &= r->body[i] == static_cast<char>('a' + i % 26);
  CHECK(pattern_ok);
  // The connection is still in sync afterwards.
  REQUIRE(c.send(get("/healthz")));
  auto again = c.read_response();
  REQUIRE(again);
  CHECK_EQ(again->body, "ok\n");
}

TEST(head_sends_headers_without_body) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send("HEAD /bytes/100 HTTP/1.1\r\nHost: t\r\n\r\n"));
  auto r = c.read_response(/*head_request=*/true);
  REQUIRE(r);
  CHECK_EQ(r->status, 200);
  CHECK_EQ(r->headers.get("Content-Length").value_or(""), "100");
  REQUIRE(c.send(get("/healthz")));  // would desync if a body had been sent
  auto next = c.read_response();
  REQUIRE(next);
  CHECK_EQ(next->body, "ok\n");
}

TEST(serves_static_files_zero_copy) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send(get("/static/hello.txt")));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 200);
  CHECK_EQ(r->body, "hello from disk\n");
  CHECK_EQ(r->headers.get("Content-Type").value_or(""), "text/plain; charset=utf-8");

  REQUIRE(c.send(get("/static/")));
  r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->body, "<h1>index</h1>\n");
  REQUIRE(c.send(get("/static/sub")));
  r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->body, "<h1>sub</h1>\n");

  REQUIRE(c.send(get("/metrics")));
  r = c.read_response();
  REQUIRE(r);
  CHECK(metric(r->body, "httpd_bytes_sendfile_total") >= 16);
}

TEST(static_files_cannot_escape_root) {
  TestServer s;
  Client c(s.port());
  const char* attacks[] = {"/static/../main.cpp", "/static/%2e%2e/%2e%2e/etc/passwd", "/static/a/../../x"};
  for (const char* path : attacks) {
    REQUIRE(c.send(get(path)));
    auto r = c.read_response();
    REQUIRE(r);
    CHECK_EQ(r->status, 400);
  }
  REQUIRE(c.send(get("/static/escape.txt")));  // symlink to /etc/hostname
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 404);
}

TEST(http10_client_gets_close_by_default) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send("GET /healthz HTTP/1.0\r\n\r\n"));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 200);
  CHECK_EQ(r->headers.get("Connection").value_or(""), "close");
  CHECK(c.closed_by_peer());
}

TEST(connection_close_is_honored) {
  TestServer s;
  Client c(s.port());
  REQUIRE(c.send(get("/healthz", "Connection: close\r\n")));
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 200);
  CHECK(c.closed_by_peer());
}

TEST(request_trickled_one_byte_per_write) {
  TestServer s;
  Client c(s.port());
  const std::string req = get("/stream/2");
  for (std::size_t i = 0; i < req.size(); ++i) {
    REQUIRE(c.send(req.substr(i, 1)));
    if (i % 8 == 0) std::this_thread::sleep_for(1ms);  // force separate TCP segments
  }
  auto r = c.read_response();
  REQUIRE(r);
  CHECK_EQ(r->status, 200);
  CHECK_EQ(r->body, "chunk 0 of 2\nchunk 1 of 2\n");
}

TEST(idle_keep_alive_connections_time_out) {
  ServerConfig cfg;
  cfg.idle_timeout = 200ms;
  TestServer s(cfg);
  Client c(s.port());
  REQUIRE(c.send(get("/healthz")));
  REQUIRE(c.read_response());
  auto start = std::chrono::steady_clock::now();
  CHECK(c.closed_by_peer());
  CHECK(std::chrono::steady_clock::now() - start < 2s);
}

TEST(slow_headers_are_cut_off) {
  ServerConfig cfg;
  cfg.header_timeout = 200ms;  // idle timeout stays at 30 s
  TestServer s(cfg);
  Client c(s.port());
  REQUIRE(c.send("GET / HTTP/1.1\r\nHost: t\r\n"));  // never finish the headers
  auto start = std::chrono::steady_clock::now();
  CHECK(c.closed_by_peer());
  CHECK(std::chrono::steady_clock::now() - start < 2s);

  Client probe(s.port());
  REQUIRE(probe.send(get("/metrics")));
  auto r = probe.read_response();
  REQUIRE(r);
  CHECK(metric(r->body, "httpd_connections_timed_out_total") >= 1);
}

TEST(connections_spread_across_reuseport_workers) {
  ServerConfig cfg;
  cfg.threads = 4;
  TestServer s(cfg);
  std::vector<std::unique_ptr<Client>> clients;
  for (int i = 0; i < 200; ++i) {
    clients.push_back(std::make_unique<Client>(s.port()));
    REQUIRE(clients.back()->send(get("/healthz")));
  }
  for (auto& c : clients) {
    auto r = c->read_response();
    REQUIRE(r);
    CHECK_EQ(r->status, 200);
  }
  Client probe(s.port());
  REQUIRE(probe.send(get("/metrics")));
  auto r = probe.read_response();
  REQUIRE(r);
  double total = 0;
  int busy_workers = 0;
  for (int w = 0; w < 4; ++w) {
    double v = metric(r->body, "httpd_connections_accepted_total{worker=\"" + std::to_string(w) + "\"}");
    total += v;
    busy_workers += v > 0;
  }
  CHECK_EQ(total, 201.0);
  CHECK(busy_workers >= 3);  // the kernel hashes connections across the group
  CHECK(metric(r->body, "httpd_request_duration_seconds_count") >= 200);
}

TEST(graceful_shutdown_drains_and_closes) {
  auto s = std::make_unique<TestServer>();
  Client idle(s->port());
  Client active(s->port());
  REQUIRE(active.send(get("/healthz")));
  REQUIRE(active.read_response());
  std::this_thread::sleep_for(20ms);  // make sure `idle` has been accepted

  auto start = std::chrono::steady_clock::now();
  s->server.stop();
  s->server.wait();
  CHECK(std::chrono::steady_clock::now() - start < 2s);  // idle conns don't hold up shutdown
  CHECK(idle.closed_by_peer());
  CHECK(active.closed_by_peer());
}

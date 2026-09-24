// loadgen: a small closed-loop HTTP/1.1 load generator (think wrk-lite).
//
// Each thread runs its own epoll loop over a share of the connections. Every
// connection keeps `pipeline` requests in flight on a keep-alive socket; each
// completed response immediately triggers the next request. Per-request
// latency is recorded in nanoseconds and reported as exact percentiles.
//
//   loadgen --port 8080 --path /healthz -c 64 -t 4 -d 10 [-p 1]

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 8080;
  std::string path = "/healthz";
  int connections = 64;
  int threads = 2;
  int duration_s = 10;
  int pipeline = 1;
};

struct ThreadStats {
  std::uint64_t requests = 0;
  std::uint64_t non_2xx = 0;
  std::uint64_t socket_errors = 0;
  std::uint64_t reconnects = 0;
  std::uint64_t bytes = 0;
  std::vector<std::uint64_t> latencies_ns;
};

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

bool iequals_prefix(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

struct Conn {
  int fd = -1;
  std::string out;
  std::size_t out_off = 0;
  std::string in;
  std::deque<std::uint64_t> sent_at;  // send timestamps of in-flight requests
};

class Worker {
 public:
  Worker(const Options& opt, int conns, Clock::time_point deadline)
      : opt_(opt), deadline_(deadline), conns_(static_cast<std::size_t>(conns)) {
    request_ = "GET " + opt.path + " HTTP/1.1\r\nHost: " + opt.host + "\r\nUser-Agent: loadgen\r\n\r\n";
    stats.latencies_ns.reserve(1 << 20);
  }

  void run() {
    ep_ = ::epoll_create1(EPOLL_CLOEXEC);
    for (std::size_t i = 0; i < conns_.size(); ++i) open(i);
    std::vector<epoll_event> events(256);
    while (Clock::now() < deadline_) {
      int n = ::epoll_wait(ep_, events.data(), static_cast<int>(events.size()), 50);
      for (int i = 0; i < n; ++i) {
        auto idx = static_cast<std::size_t>(events[i].data.u64);
        if (!service(conns_[idx], events[i].events)) {
          ++stats.socket_errors;
          reopen(idx);
        }
      }
    }
    for (auto& c : conns_) {
      if (c.fd >= 0) ::close(c.fd);
    }
    ::close(ep_);
  }

  ThreadStats stats;

 private:
  void open(std::size_t idx) {
    Conn& c = conns_[idx];
    c = Conn{};
    c.fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    ::setsockopt(c.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(opt_.port);
    ::inet_pton(AF_INET, opt_.host.c_str(), &sa.sin_addr);
    ::connect(c.fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa);  // EINPROGRESS
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.u64 = idx;
    ::epoll_ctl(ep_, EPOLL_CTL_ADD, c.fd, &ev);
    for (int i = 0; i < opt_.pipeline; ++i) enqueue(c);
  }

  void reopen(std::size_t idx) {
    ::close(conns_[idx].fd);
    ++stats.reconnects;
    open(idx);
  }

  void enqueue(Conn& c) {
    c.out += request_;
    c.sent_at.push_back(now_ns());
  }

  bool service(Conn& c, std::uint32_t events) {
    if (events & EPOLLERR) return false;
    char buf[64 * 1024];
    for (;;) {  // drain input (edge-triggered)
      ssize_t n = ::recv(c.fd, buf, sizeof buf, 0);
      if (n > 0) {
        c.in.append(buf, static_cast<std::size_t>(n));
        stats.bytes += static_cast<std::uint64_t>(n);
        continue;
      }
      if (n == 0) return false;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EINTR) continue;
      return false;
    }
    if (!consume_responses(c)) return false;
    while (c.out_off < c.out.size()) {
      ssize_t n = ::send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
      if (n > 0) {
        c.out_off += static_cast<std::size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN)) break;
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    if (c.out_off == c.out.size()) {
      c.out.clear();
      c.out_off = 0;
    }
    return true;
  }

  // Parses as many complete Content-Length framed responses as are buffered.
  bool consume_responses(Conn& c) {
    std::size_t pos = 0;
    for (;;) {
      std::size_t head_end = c.in.find("\r\n\r\n", pos);
      if (head_end == std::string::npos) break;
      std::string_view head(c.in.data() + pos, head_end - pos);
      if (head.size() < 12) return false;
      int status = std::atoi(std::string(head.substr(9, 3)).c_str());
      std::size_t length = 0;
      bool have_length = false;
      std::size_t line = head.find("\r\n");
      while (line != std::string_view::npos) {
        std::string_view rest = head.substr(line + 2);
        if (iequals_prefix(rest, "content-length:")) {
          length = std::strtoull(std::string(rest.substr(15, rest.find("\r\n") - 15)).c_str(), nullptr, 10);
          have_length = true;
        }
        line = head.find("\r\n", line + 2);
      }
      if (!have_length) {
        std::fprintf(stderr, "loadgen: only Content-Length responses are supported\n");
        std::exit(1);
      }
      std::size_t total = head_end + 4 - pos + length;
      if (c.in.size() - pos < total) break;
      pos += total;

      if (c.sent_at.empty()) return false;  // response nobody asked for
      stats.latencies_ns.push_back(now_ns() - c.sent_at.front());
      c.sent_at.pop_front();
      ++stats.requests;
      if (status < 200 || status >= 300) ++stats.non_2xx;
      if (Clock::now() < deadline_) enqueue(c);
    }
    c.in.erase(0, pos);
    return true;
  }

  const Options& opt_;
  Clock::time_point deadline_;
  std::vector<Conn> conns_;
  std::string request_;
  int ep_ = -1;
};

std::string format_ns(std::uint64_t ns) {
  char buf[32];
  if (ns < 1'000'000) {
    std::snprintf(buf, sizeof buf, "%.1fus", static_cast<double>(ns) / 1e3);
  } else {
    std::snprintf(buf, sizeof buf, "%.2fms", static_cast<double>(ns) / 1e6);
  }
  return buf;
}

void usage() {
  std::printf(
      "usage: loadgen [--host 127.0.0.1] [--port 8080] [--path /healthz]\n"
      "               [-c connections] [-t threads] [-d seconds] [-p pipeline-depth]\n");
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : nullptr;
    auto num = [&] {
      if (!v) {
        usage();
        std::exit(2);
      }
      ++i;
      return std::atoi(v);
    };
    if (a == "--host" && v) {
      opt.host = v;
      ++i;
    } else if (a == "--port") {
      opt.port = static_cast<std::uint16_t>(num());
    } else if (a == "--path" && v) {
      opt.path = v;
      ++i;
    } else if (a == "-c") {
      opt.connections = num();
    } else if (a == "-t") {
      opt.threads = num();
    } else if (a == "-d") {
      opt.duration_s = num();
    } else if (a == "-p") {
      opt.pipeline = num();
    } else {
      usage();
      return a == "-h" || a == "--help" ? 0 : 2;
    }
  }
  opt.threads = std::max(1, std::min(opt.threads, opt.connections));
  opt.pipeline = std::max(1, opt.pipeline);

  std::printf("loadgen: %ds against http://%s:%u%s, %d connections, %d threads, pipeline %d\n", opt.duration_s,
              opt.host.c_str(), opt.port, opt.path.c_str(), opt.connections, opt.threads, opt.pipeline);

  const auto start = Clock::now();
  const auto deadline = start + std::chrono::seconds(opt.duration_s);
  std::vector<std::unique_ptr<Worker>> workers;
  std::vector<std::thread> threads;
  for (int t = 0; t < opt.threads; ++t) {
    int share = opt.connections / opt.threads + (t < opt.connections % opt.threads ? 1 : 0);
    workers.push_back(std::make_unique<Worker>(opt, share, deadline));
  }
  for (auto& w : workers) threads.emplace_back([&w] { w->run(); });
  for (auto& t : threads) t.join();
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

  ThreadStats all;
  for (auto& w : workers) {
    all.requests += w->stats.requests;
    all.non_2xx += w->stats.non_2xx;
    all.socket_errors += w->stats.socket_errors;
    all.reconnects += w->stats.reconnects;
    all.bytes += w->stats.bytes;
    all.latencies_ns.insert(all.latencies_ns.end(), w->stats.latencies_ns.begin(), w->stats.latencies_ns.end());
  }
  std::sort(all.latencies_ns.begin(), all.latencies_ns.end());
  auto pct = [&](double p) -> std::uint64_t {
    if (all.latencies_ns.empty()) return 0;
    auto idx = static_cast<std::size_t>(p / 100.0 * static_cast<double>(all.latencies_ns.size() - 1));
    return all.latencies_ns[idx];
  };

  std::printf("  requests      %llu in %.2fs\n", static_cast<unsigned long long>(all.requests), elapsed);
  std::printf("  throughput    %.0f req/s, %.2f MiB/s\n", static_cast<double>(all.requests) / elapsed,
              static_cast<double>(all.bytes) / elapsed / (1024.0 * 1024.0));
  std::printf("  latency       p50 %s  p90 %s  p99 %s  p99.9 %s  max %s\n", format_ns(pct(50)).c_str(),
              format_ns(pct(90)).c_str(), format_ns(pct(99)).c_str(), format_ns(pct(99.9)).c_str(),
              format_ns(all.latencies_ns.empty() ? 0 : all.latencies_ns.back()).c_str());
  std::printf("  errors        non-2xx %llu, socket %llu, reconnects %llu\n",
              static_cast<unsigned long long>(all.non_2xx), static_cast<unsigned long long>(all.socket_errors),
              static_cast<unsigned long long>(all.reconnects));
  return all.socket_errors == 0 && all.non_2xx == 0 ? 0 : 1;
}

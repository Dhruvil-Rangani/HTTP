#include "server/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>

#include "obs/metrics.hpp"
#include "util/byte_buffer.hpp"
#include "util/unique_fd.hpp"

namespace httpd {
namespace {

using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::steady_clock;

constexpr std::size_t kReadSize = 16 * 1024;
constexpr std::size_t kOutHighWater = 256 * 1024;     // stop dispatching pipelined requests above this
constexpr std::size_t kProducerLowWater = 64 * 1024;  // refill streaming bodies up to this much
constexpr std::size_t kSendfileMax = 1 << 20;         // per call, so one huge file can't starve the loop
constexpr int kMaxEvents = 256;
constexpr int kAcceptBatch = 64;

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

std::uint64_t mono_us() {
  return static_cast<std::uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

std::uint64_t to_us(std::chrono::milliseconds ms) {
  return static_cast<std::uint64_t>(ms.count()) * 1000;
}

UniqueFd make_listener(const std::string& address, std::uint16_t port, int backlog) {
  UniqueFd fd(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (!fd.valid()) throw_errno("socket");
  int one = 1;
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0) throw_errno("SO_REUSEADDR");
  if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) < 0) throw_errno("SO_REUSEPORT");

  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (::inet_pton(AF_INET, address.c_str(), &sa.sin_addr) != 1) {
    throw std::invalid_argument("invalid IPv4 bind address: " + address);
  }
  if (::bind(fd.get(), reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0) throw_errno("bind");
  if (::listen(fd.get(), backlog) < 0) throw_errno("listen");
  return fd;
}

std::uint16_t bound_port(int fd) {
  sockaddr_in sa{};
  socklen_t len = sizeof sa;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &len) < 0) throw_errno("getsockname");
  return ntohs(sa.sin_port);
}

// CPUs this process may run on (respects taskset / cgroup cpusets).
std::vector<int> allowed_cpus() {
  std::vector<int> cpus;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::sched_getaffinity(0, sizeof set, &set) == 0) {
    for (int i = 0; i < CPU_SETSIZE; ++i) {
      if (CPU_ISSET(i, &set)) cpus.push_back(i);
    }
  }
  return cpus;
}

// The Date header only changes once per second, so format it once per second
// per worker instead of once per response.
class DateCache {
 public:
  std::string_view get(std::time_t now) {
    if (now != cached_) {
      std::tm tm{};
      ::gmtime_r(&now, &tm);
      len_ = std::strftime(buf_, sizeof buf_, "%a, %d %b %Y %H:%M:%S GMT", &tm);
      cached_ = now;
    }
    return {buf_, len_};
  }

 private:
  std::time_t cached_ = -1;
  char buf_[64] = {};
  std::size_t len_ = 0;
};

struct Connection {
  Connection(int fd_in, const ParserLimits& limits, std::uint64_t now) : fd(fd_in), parser(limits), last_active_us(now) {}

  UniqueFd fd;
  ByteBuffer in;
  RequestParser parser;
  std::string out;  // serialized bytes not yet accepted by the kernel
  std::size_t out_off = 0;

  // Body source of the response in flight; transmitted after `out` drains.
  UniqueFd file;
  off_t file_off = 0;
  std::uint64_t file_left = 0;
  BodyProducer producer;
  Headers trailers;
  bool chunked = false;
  bool stream_sized = false;
  std::uint64_t stream_left = 0;

  // Edge-triggered readiness we have not yet exhausted.
  bool can_read = true;
  bool can_write = true;
  bool peer_closed = false;        // recv() returned 0
  bool close_after_write = false;  // Connection: close, error response, or drain
  bool continue_sent = false;      // "100 Continue" already sent for this request
  std::uint64_t last_active_us;
  std::uint64_t request_start_us = 0;  // first byte of the request being parsed; 0 = none

  std::size_t out_pending() const noexcept { return out.size() - out_off; }
  bool body_source_active() const noexcept { return file.valid() || static_cast<bool>(producer); }
  bool has_output() const noexcept { return out_pending() > 0 || body_source_active(); }
};

}  // namespace

class Server::Worker {
 public:
  Worker(Server& server, unsigned id, UniqueFd listener, int cpu);
  void start() { thread_ = std::thread([this] { run(); }); }
  void join() {
    if (thread_.joinable()) thread_.join();
  }
  void request_stop() noexcept {
    std::uint64_t one = 1;
    [[maybe_unused]] ssize_t r = ::write(wake_.get(), &one, sizeof one);
  }
  const WorkerMetrics& metrics() const noexcept { return metrics_; }

 private:
  void run();
  void on_accept();
  bool drive(Connection& c);
  bool do_read(Connection& c, bool& progress);
  bool process(Connection& c);
  bool do_write(Connection& c, bool& progress);
  bool refill_from_producer(Connection& c);
  void dispatch(Connection& c, Request req);
  void queue_error(Connection& c, int status, const std::string& detail);
  void record_response(Connection& c, int status);
  void close_connection(int fd);
  void sweep();
  void begin_drain();

  Server& server_;
  const unsigned id_;
  const int cpu_;
  const std::size_t max_in_buffer_;
  const std::uint64_t idle_timeout_us_;
  const std::uint64_t header_timeout_us_;
  const std::uint64_t tick_us_;
  UniqueFd listener_;
  UniqueFd epoll_;
  UniqueFd wake_;  // eventfd used by stop() to interrupt epoll_wait
  std::unordered_map<int, std::unique_ptr<Connection>> conns_;
  WorkerMetrics metrics_;
  DateCache date_;
  std::uint64_t now_us_ = 0;  // cached per loop iteration
  bool draining_ = false;
  std::uint64_t drain_deadline_us_ = 0;
  std::thread thread_;
};

Server::Worker::Worker(Server& server, unsigned id, UniqueFd listener, int cpu)
    : server_(server),
      id_(id),
      cpu_(cpu),
      max_in_buffer_(std::max<std::size_t>(64 * 1024, server.cfg_.limits.max_request_line +
                                                           server.cfg_.limits.max_header_bytes)),
      idle_timeout_us_(to_us(server.cfg_.idle_timeout)),
      header_timeout_us_(to_us(server.cfg_.header_timeout)),
      // Timeouts are enforced by a periodic sweep; run it often enough that a
      // deadline is overshot by at most ~25%.
      tick_us_(std::clamp<std::uint64_t>(std::min(idle_timeout_us_, header_timeout_us_) / 4, 10'000, 1'000'000)),
      listener_(std::move(listener)),
      now_us_(mono_us()) {
  epoll_.reset(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_.valid()) throw_errno("epoll_create1");
  wake_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
  if (!wake_.valid()) throw_errno("eventfd");

  // The listener is level-triggered: if more than kAcceptBatch connections are
  // queued we are simply woken again, which keeps accepting fair to I/O.
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = listener_.get();
  if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, listener_.get(), &ev) < 0) throw_errno("epoll_ctl(listener)");
  ev.data.fd = wake_.get();
  if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, wake_.get(), &ev) < 0) throw_errno("epoll_ctl(eventfd)");
}

void Server::Worker::run() {
  char name[16];
  std::snprintf(name, sizeof name, "httpd-w%u", id_);
  ::pthread_setname_np(::pthread_self(), name);  // visible in top -H, perf, gdb
  if (cpu_ >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_, &set);
    if (int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof set, &set); rc != 0) {
      std::fprintf(stderr, "httpd: worker %u: cannot pin to cpu %d: %s\n", id_, cpu_, std::strerror(rc));
    }
  }

  std::array<epoll_event, kMaxEvents> events;
  std::uint64_t last_sweep = mono_us();
  const int timeout_ms = static_cast<int>(tick_us_ / 1000);

  for (;;) {
    int n = ::epoll_wait(epoll_.get(), events.data(), kMaxEvents, timeout_ms);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::perror("httpd: epoll_wait");
      break;
    }
    now_us_ = mono_us();
    bump(metrics_.loop_wakeups);
    bump(metrics_.loop_events, static_cast<std::uint64_t>(n));

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      const std::uint32_t ev = events[i].events;
      if (fd == wake_.get()) {
        std::uint64_t v;
        [[maybe_unused]] ssize_t r = ::read(wake_.get(), &v, sizeof v);
        begin_drain();
        continue;
      }
      if (listener_.valid() && fd == listener_.get()) {
        on_accept();
        continue;
      }
      // Look up by fd rather than trusting a pointer in epoll_data: a
      // connection closed earlier in this batch can never be touched again.
      auto it = conns_.find(fd);
      if (it == conns_.end()) continue;
      Connection& c = *it->second;
      if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) c.can_read = true;
      if (ev & (EPOLLOUT | EPOLLHUP | EPOLLERR)) c.can_write = true;
      if (!drive(c)) close_connection(fd);
    }

    if (draining_ || now_us_ - last_sweep >= tick_us_) {
      sweep();
      last_sweep = now_us_;
    }
    if (draining_ && (conns_.empty() || now_us_ >= drain_deadline_us_)) break;
  }

  while (!conns_.empty()) close_connection(conns_.begin()->first);
}

void Server::Worker::on_accept() {
  for (int i = 0; i < kAcceptBatch; ++i) {
    int fd = ::accept4(listener_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) bump(metrics_.accept_errors);  // EMFILE, ENOBUFS, ...
      return;
    }
    // Responses are written in one go, so Nagle would only add latency.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    auto conn = std::make_unique<Connection>(fd, server_.cfg_.limits, now_us_);
    // Edge-triggered and registered once for both directions: we track
    // readiness ourselves (can_read / can_write) and never need EPOLL_CTL_MOD
    // syscalls to toggle EPOLLOUT interest.
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    ev.data.fd = fd;
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
      bump(metrics_.accept_errors);
      continue;  // `conn` closes the socket
    }
    bump(metrics_.connections_accepted);
    conns_.emplace(fd, std::move(conn));
  }
}

// Runs read -> parse/dispatch -> write until nothing makes progress (the
// socket would block in both directions or there is simply nothing to do).
// Returns false when the connection must be closed.
bool Server::Worker::drive(Connection& c) {
  for (;;) {
    bool progress = false;
    if (c.can_read && !c.peer_closed && !c.close_after_write && c.in.size() < max_in_buffer_) {
      if (!do_read(c, progress)) return false;
    }
    if (process(c)) progress = true;
    if (c.can_write && c.has_output()) {
      if (!do_write(c, progress)) return false;
    }
    if (!progress) break;
  }

  if (c.has_output()) return true;  // wait for EPOLLOUT
  if (c.close_after_write) {
    // Send our FIN first, then discard anything the client already sent:
    // closing with unread data makes the kernel send RST, which can destroy
    // the response we just wrote before the client reads it.
    ::shutdown(c.fd.get(), SHUT_WR);
    char sink[4096];
    while (::recv(c.fd.get(), sink, sizeof sink, 0) > 0) {
    }
    return false;
  }
  return !c.peer_closed;
}

bool Server::Worker::do_read(Connection& c, bool& progress) {
  // Edge-triggered: keep reading until EAGAIN or we would not get another event.
  while (c.in.size() < max_in_buffer_) {
    char* dst = c.in.prepare(kReadSize);
    ssize_t n = ::recv(c.fd.get(), dst, kReadSize, 0);
    if (n > 0) {
      c.in.commit(static_cast<std::size_t>(n));
      bump(metrics_.bytes_received, static_cast<std::uint64_t>(n));
      c.last_active_us = now_us_;
      progress = true;
      continue;
    }
    if (n == 0) {
      c.peer_closed = true;
      progress = true;
      return true;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      c.can_read = false;
      return true;
    }
    return false;  // ECONNRESET, ETIMEDOUT, ...
  }
  return true;  // buffer full: resume once the parser has consumed some input
}

bool Server::Worker::process(Connection& c) {
  bool progress = false;
  // Responses must go out in request order. While a file/stream body is being
  // transmitted, or too much output is queued, pipelined requests wait in `in`.
  while (!c.close_after_write && !c.body_source_active() && c.out_pending() < kOutHighWater) {
    std::string_view data = c.in.readable();
    if (data.empty()) break;
    if (c.request_start_us == 0) c.request_start_us = now_us_;

    std::size_t used = c.parser.feed(data);
    c.in.consume(used);
    if (used) progress = true;

    if (c.parser.failed()) {
      bump(metrics_.parse_errors);
      queue_error(c, status_for(c.parser.error()), c.parser.error_detail());
      return true;
    }
    if (c.parser.request().expect_continue && !c.continue_sent && c.parser.headers_complete() &&
        !c.parser.done()) {
      c.out.append("HTTP/1.1 100 Continue\r\n\r\n");
      c.continue_sent = true;
      progress = true;
    }
    if (!c.parser.done()) break;  // need more bytes

    dispatch(c, c.parser.take());
    c.continue_sent = false;
    progress = true;
  }
  return progress;
}

void Server::Worker::dispatch(Connection& c, Request req) {
  Response res;
  server_.router_.dispatch(req, res);

  HeadOptions opts;
  opts.keep_alive = req.keep_alive() && !draining_;
  opts.http10_client = req.is_http10();
  opts.date = date_.get(std::time(nullptr));
  const Framing framing = write_head(c.out, res, opts);
  if (!opts.keep_alive || framing == Framing::CloseDelimited) c.close_after_write = true;

  if (req.method != "HEAD" && framing != Framing::None) {
    if (res.file.valid()) {
      if (res.file_length > 0) {
        c.file = std::move(res.file);
        c.file_off = static_cast<off_t>(res.file_offset);
        c.file_left = res.file_length;
      }
    } else if (res.producer) {
      c.chunked = framing == Framing::Chunked;
      c.stream_sized = res.stream_length.has_value();
      c.stream_left = res.stream_length.value_or(0);
      c.trailers.clear();
      if (!(c.stream_sized && c.stream_left == 0)) c.producer = std::move(res.producer);
    } else {
      c.out.append(res.body);
    }
  }
  record_response(c, res.status);
}

void Server::Worker::queue_error(Connection& c, int status, const std::string& detail) {
  Response res = Response::text(status, std::string(reason_phrase(status)) + ": " + detail + "\n");
  HeadOptions opts;
  opts.keep_alive = false;  // after a framing error we cannot know where the next request starts
  opts.date = date_.get(std::time(nullptr));
  write_head(c.out, res, opts);
  c.out.append(res.body);
  c.close_after_write = true;
  record_response(c, status);
}

void Server::Worker::record_response(Connection& c, int status) {
  bump(metrics_.requests);
  int cls = status / 100;
  if (cls >= 1 && cls <= 5) bump(metrics_.responses_by_class[static_cast<std::size_t>(cls)]);
  if (c.request_start_us) metrics_.observe_latency_us(mono_us() - c.request_start_us);
  c.request_start_us = 0;
}

bool Server::Worker::do_write(Connection& c, bool& progress) {
  while (c.can_write) {
    if (c.out_pending() > 0) {
      const std::size_t want = c.out_pending();
      ssize_t n = ::send(c.fd.get(), c.out.data() + c.out_off, want, MSG_NOSIGNAL);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          c.can_write = false;
          break;
        }
        return false;  // EPIPE, ECONNRESET, ...
      }
      const auto sent = static_cast<std::size_t>(n);
      c.out_off += sent;
      bump(metrics_.bytes_sent, sent);
      c.last_active_us = now_us_;
      progress = true;
      if (c.out_off == c.out.size()) {
        c.out.clear();  // keeps capacity: no reallocation for the next response
        c.out_off = 0;
      } else if (c.out_off > c.out.size() / 2) {
        c.out.erase(0, c.out_off);
        c.out_off = 0;
      }
      // A short write means the socket send buffer is full; skip the send()
      // that would just return EAGAIN and wait for EPOLLOUT instead.
      if (sent < want) {
        c.can_write = false;
        break;
      }
      continue;
    }

    if (c.file.valid()) {
      // Zero-copy: pages go from the page cache to the socket without ever
      // being copied into user space.
      off_t off = c.file_off;
      const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(c.file_left, kSendfileMax));
      ssize_t n = ::sendfile(c.fd.get(), c.file.get(), &off, want);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          c.can_write = false;
          break;
        }
        return false;
      }
      if (n == 0) return false;  // file shrank under us: the promised Content-Length is now a lie
      const auto sent = static_cast<std::uint64_t>(n);
      c.file_off = off;
      c.file_left -= sent;
      bump(metrics_.bytes_sent, sent);
      bump(metrics_.bytes_sendfile, sent);
      c.last_active_us = now_us_;
      progress = true;
      if (c.file_left == 0) {
        c.file.reset();
      } else if (sent < want) {
        c.can_write = false;
        break;
      }
      continue;
    }

    if (c.producer) {
      if (!refill_from_producer(c)) return false;
      progress = true;
      continue;
    }
    break;
  }
  return true;
}

// Pulls up to kProducerLowWater bytes from the streaming body into `out`.
// Only called once `out` has fully drained, i.e. when the client is keeping up.
bool Server::Worker::refill_from_producer(Connection& c) {
  std::string piece;
  bool more = true;
  try {
    while (more && c.out.size() < kProducerLowWater) {
      piece.clear();
      more = c.producer(piece, c.trailers);
      if (more && piece.empty()) return false;  // contract violation: would spin forever
      if (c.stream_sized) {
        if (piece.size() > c.stream_left) return false;  // more than the declared Content-Length
        c.stream_left -= piece.size();
        c.out.append(piece);
        if (c.stream_left == 0) more = false;
      } else if (c.chunked) {
        append_chunk(c.out, piece);
      } else {
        c.out.append(piece);  // close-delimited (HTTP/1.0)
      }
    }
  } catch (const std::exception&) {
    // The status line is already on the wire; aborting the connection is the
    // only way left to tell the client the body is incomplete.
    return false;
  }
  if (!more) {
    c.producer = nullptr;
    if (c.chunked) append_last_chunk(c.out, c.trailers);
    if (c.stream_sized && c.stream_left != 0) c.close_after_write = true;  // body came up short
    c.trailers.clear();
  }
  return true;
}

void Server::Worker::close_connection(int fd) {
  if (conns_.erase(fd)) bump(metrics_.connections_closed);  // ~Connection closes the socket
}

void Server::Worker::sweep() {
  for (auto it = conns_.begin(); it != conns_.end();) {
    const Connection& c = *it->second;
    const bool idle = !c.has_output() && c.in.empty() && !c.parser.started();
    bool expired = false;
    if (draining_ && idle) {
      expired = true;  // drop idle keep-alive connections as soon as we drain
    } else if (c.request_start_us && !c.parser.headers_complete() &&
               now_us_ - c.request_start_us > header_timeout_us_) {
      expired = true;  // slowloris: headers trickling in forever
      bump(metrics_.connections_timed_out);
    } else if (now_us_ - c.last_active_us > idle_timeout_us_) {
      expired = true;  // idle keep-alive, or a peer that stopped reading
      bump(metrics_.connections_timed_out);
    }
    if (expired) {
      bump(metrics_.connections_closed);
      it = conns_.erase(it);
    } else {
      ++it;
    }
  }
}

void Server::Worker::begin_drain() {
  if (draining_) return;
  draining_ = true;
  drain_deadline_us_ = now_us_ + to_us(server_.cfg_.shutdown_grace);
  if (listener_.valid()) {
    ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, listener_.get(), nullptr);
    listener_.reset();  // new connections now go to the other workers or are refused
  }
}

Server::Server(ServerConfig cfg) : cfg_(std::move(cfg)) {}

Server::~Server() {
  stop();
  wait();
}

void Server::start() {
  if (!workers_.empty()) throw std::logic_error("server already started");
  // sendfile(2) has no MSG_NOSIGNAL: without this a client that disconnects
  // mid-transfer would kill the whole process with SIGPIPE.
  std::signal(SIGPIPE, SIG_IGN);

  const unsigned n = cfg_.threads ? cfg_.threads : std::max(1u, std::thread::hardware_concurrency());
  const std::vector<int> cpus = cfg_.pin_threads ? allowed_cpus() : std::vector<int>{};
  auto cpu_for = [&](unsigned i) { return cpus.empty() ? -1 : cpus[i % cpus.size()]; };

  // The first socket fixes the port (this makes port 0 work); the others join
  // its SO_REUSEPORT group and the kernel hashes connections across them.
  UniqueFd first = make_listener(cfg_.bind_address, cfg_.port, cfg_.backlog);
  port_ = bound_port(first.get());
  started_at_ = steady_clock::now();

  workers_.reserve(n);
  workers_.push_back(std::make_unique<Worker>(*this, 0, std::move(first), cpu_for(0)));
  for (unsigned i = 1; i < n; ++i) {
    workers_.push_back(
        std::make_unique<Worker>(*this, i, make_listener(cfg_.bind_address, port_, cfg_.backlog), cpu_for(i)));
  }
  for (auto& w : workers_) w->start();
}

void Server::stop() {
  for (auto& w : workers_) w->request_stop();
}

void Server::wait() {
  for (auto& w : workers_) w->join();
}

std::string Server::metrics_text() const {
  std::vector<const WorkerMetrics*> ws;
  ws.reserve(workers_.size());
  for (const auto& w : workers_) ws.push_back(&w->metrics());
  const double uptime = std::chrono::duration<double>(steady_clock::now() - started_at_).count();
  return render_prometheus(ws, uptime);
}

}  // namespace httpd

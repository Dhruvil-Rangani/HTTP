#include "obs/metrics.hpp"

#include <cstdio>

namespace httpd {
namespace {

std::uint64_t load(const std::atomic<std::uint64_t>& c) { return c.load(std::memory_order_relaxed); }

void header(std::string& out, const char* name, const char* type, const char* help) {
  out.append("# HELP ").append(name).append(" ").append(help).append("\n");
  out.append("# TYPE ").append(name).append(" ").append(type).append("\n");
}

void sample(std::string& out, const char* name, const std::string& labels, double value) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", value);
  out.append(name);
  if (!labels.empty()) out.append("{").append(labels).append("}");
  out.append(" ").append(buf).append("\n");
}

using Field = std::atomic<std::uint64_t> WorkerMetrics::*;

// A counter that is also broken down per worker, which makes it easy to see
// how evenly SO_REUSEPORT spreads connections across the event loops.
void per_worker(std::string& out, const std::vector<const WorkerMetrics*>& ws, const char* name,
                const char* type, const char* help, Field field) {
  header(out, name, type, help);
  for (std::size_t i = 0; i < ws.size(); ++i) {
    sample(out, name, "worker=\"" + std::to_string(i) + "\"", static_cast<double>(load(ws[i]->*field)));
  }
}

void total(std::string& out, const std::vector<const WorkerMetrics*>& ws, const char* name,
           const char* help, Field field) {
  std::uint64_t sum = 0;
  for (const auto* w : ws) sum += load(w->*field);
  header(out, name, "counter", help);
  sample(out, name, "", static_cast<double>(sum));
}

}  // namespace

std::string render_prometheus(const std::vector<const WorkerMetrics*>& ws, double uptime_seconds) {
  std::string out;
  out.reserve(4096);

  header(out, "httpd_uptime_seconds", "gauge", "Seconds since the server started.");
  sample(out, "httpd_uptime_seconds", "", uptime_seconds);

  header(out, "httpd_connections_active", "gauge", "Open client connections per worker.");
  for (std::size_t i = 0; i < ws.size(); ++i) {
    // Two independent relaxed loads can race with the writer; clamp so a
    // scrape can never report a wrapped-around negative value.
    std::uint64_t closed = load(ws[i]->connections_closed);
    std::uint64_t accepted = load(ws[i]->connections_accepted);
    double active = accepted >= closed ? static_cast<double>(accepted - closed) : 0.0;
    sample(out, "httpd_connections_active", "worker=\"" + std::to_string(i) + "\"", active);
  }
  per_worker(out, ws, "httpd_connections_accepted_total", "counter", "Accepted TCP connections.",
             &WorkerMetrics::connections_accepted);
  per_worker(out, ws, "httpd_requests_total", "counter", "Requests dispatched to a handler.",
             &WorkerMetrics::requests);
  total(out, ws, "httpd_connections_timed_out_total", "Connections closed by idle/header timeouts.",
        &WorkerMetrics::connections_timed_out);
  total(out, ws, "httpd_accept_errors_total", "accept4() failures (EMFILE, ENOBUFS, ...).",
        &WorkerMetrics::accept_errors);
  total(out, ws, "httpd_parse_errors_total", "Requests rejected by the HTTP parser.",
        &WorkerMetrics::parse_errors);
  total(out, ws, "httpd_bytes_received_total", "Bytes read from client sockets.", &WorkerMetrics::bytes_received);
  total(out, ws, "httpd_bytes_sent_total", "Bytes written to client sockets (all paths).",
        &WorkerMetrics::bytes_sent);
  total(out, ws, "httpd_bytes_sendfile_total", "Bytes sent zero-copy via sendfile(2).",
        &WorkerMetrics::bytes_sendfile);
  per_worker(out, ws, "httpd_eventloop_wakeups_total", "counter", "epoll_wait() returns.",
             &WorkerMetrics::loop_wakeups);
  per_worker(out, ws, "httpd_eventloop_events_total", "counter", "Events delivered by epoll_wait().",
             &WorkerMetrics::loop_events);

  header(out, "httpd_responses_total", "counter", "Responses by status class.");
  for (std::size_t cls = 1; cls <= 5; ++cls) {
    std::uint64_t sum = 0;
    for (const auto* w : ws) sum += load(w->responses_by_class[cls]);
    sample(out, "httpd_responses_total", "code=\"" + std::to_string(cls) + "xx\"", static_cast<double>(sum));
  }

  header(out, "httpd_request_duration_seconds", "histogram",
         "Time from the first byte of a request until its response is queued.");
  std::uint64_t cumulative = 0;
  for (std::size_t b = 0; b < kLatencyBuckets; ++b) {
    for (const auto* w : ws) cumulative += load(w->latency_buckets[b]);
    if (b + 1 == kLatencyBuckets) break;  // the last bucket is open-ended: only +Inf
    char le[32];
    std::snprintf(le, sizeof le, "le=\"%g\"", static_cast<double>(1ull << b) / 1e6);
    sample(out, "httpd_request_duration_seconds_bucket", le, static_cast<double>(cumulative));
  }
  sample(out, "httpd_request_duration_seconds_bucket", "le=\"+Inf\"", static_cast<double>(cumulative));
  std::uint64_t sum_us = 0;
  for (const auto* w : ws) sum_us += load(w->latency_sum_us);
  sample(out, "httpd_request_duration_seconds_sum", "", static_cast<double>(sum_us) / 1e6);
  sample(out, "httpd_request_duration_seconds_count", "", static_cast<double>(cumulative));
  return out;
}

}  // namespace httpd

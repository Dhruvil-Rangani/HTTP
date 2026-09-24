#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace httpd {

// Adds to a counter that has exactly one writer thread. A plain load + store
// compiles to ordinary movs; fetch_add would emit a `lock`-prefixed RMW that
// costs tens of cycles on every request for no benefit here.
inline void bump(std::atomic<std::uint64_t>& counter, std::uint64_t n = 1) noexcept {
  counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

// Latency buckets are powers of two in microseconds: bucket i counts samples
// with bit_width(us) == i, i.e. us < 2^i. 24 buckets reach ~8.4 s.
constexpr std::size_t kLatencyBuckets = 24;

// Counters for one event-loop worker. Each worker is the sole writer of its own
// block; a /metrics scrape on any thread only reads. alignas(64) puts every
// block on its own cache lines so workers never false-share.
struct alignas(64) WorkerMetrics {
  std::atomic<std::uint64_t> connections_accepted{0};
  std::atomic<std::uint64_t> connections_closed{0};
  std::atomic<std::uint64_t> connections_timed_out{0};
  std::atomic<std::uint64_t> accept_errors{0};
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> parse_errors{0};
  std::array<std::atomic<std::uint64_t>, 6> responses_by_class{};  // index = status / 100
  std::atomic<std::uint64_t> bytes_received{0};
  std::atomic<std::uint64_t> bytes_sent{0};
  std::atomic<std::uint64_t> bytes_sendfile{0};
  std::atomic<std::uint64_t> loop_wakeups{0};
  std::atomic<std::uint64_t> loop_events{0};
  std::array<std::atomic<std::uint64_t>, kLatencyBuckets> latency_buckets{};
  std::atomic<std::uint64_t> latency_sum_us{0};

  void observe_latency_us(std::uint64_t us) noexcept {
    std::size_t idx = us == 0 ? 0 : static_cast<std::size_t>(64 - __builtin_clzll(us));
    if (idx >= kLatencyBuckets) idx = kLatencyBuckets - 1;
    bump(latency_buckets[idx]);
    bump(latency_sum_us, us);
  }
};

// Renders all workers in the Prometheus text exposition format (v0.0.4).
std::string render_prometheus(const std::vector<const WorkerMetrics*>& workers, double uptime_seconds);

}  // namespace httpd

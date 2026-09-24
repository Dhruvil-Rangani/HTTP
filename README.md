# httpd-cpp

A multi-threaded HTTP/1.1 server written from scratch in C++17 on raw Linux sockets. No frameworks or third-party libraries: just `socket`/`epoll`/`sendfile` and the C++ standard library.

The core is an incremental parser fed from a raw byte stream. Around it sits the systems work: an event loop, zero-copy I/O, backpressure, timeouts, observability, fuzzing and benchmarking.

```
$ make && ./build/httpd --threads 4 --root public
httpd listening on 0.0.0.0:8080 with 4 worker(s)
```

## Highlights

| Area | What's implemented | Where |
|---|---|---|
| Socket programming | Non-blocking sockets, edge-triggered `epoll`, `accept4`, `TCP_NODELAY`, half-close handling, lingering close (FIN, then drain, so a response is never destroyed by an RST) | [src/server/server.cpp](src/server/server.cpp) |
| Scalability | One event loop per core, each with its own `SO_REUSEPORT` listener: no shared accept lock, no cross-thread handoff, optional CPU pinning (`--pin`) | `Server::start`, `Worker::run` |
| Zero-copy I/O | Static files go page cache → socket with `sendfile(2)` | `Worker::do_write` |
| Backpressure | Streaming bodies are *pulled* only once the socket drains, so a slow client can't make the server buffer a 1 GiB response | `BodyProducer`, `refill_from_producer` |
| Protocol correctness | RFC 9112 parser: keep-alive, pipelining, chunked request/response bodies with trailers, `Expect: 100-continue`, HEAD, HTTP/1.0 | [src/http/](src/http/) |
| Reliability/security | Size limits (414/431/413), header timeout (slowloris), idle timeout, request-smuggling guards (CL+TE, duplicate CL, bare LF/NUL), `openat2(RESOLVE_BENEATH)` path-traversal protection, graceful drain on SIGTERM | parser, `sweep()`, [static_files.cpp](src/app/static_files.cpp) |
| Observability | Prometheus `/metrics`: per-worker lock-free counters on separate cache lines, latency histogram, event-loop wakeups/events, sendfile bytes; `/debug/info` for bring-up checks | [src/obs/metrics.hpp](src/obs/metrics.hpp) |
| Testing & debugging | 52 unit + end-to-end tests, ASan/UBSan, TSan, a differential fuzzer, CI | [tests/](tests/), [fuzz/](fuzz/) |
| Tooling | `loadgen`: a multi-threaded epoll load generator with pipelining and exact p50/p99/p99.9 latencies | [tools/loadgen.cpp](tools/loadgen.cpp) |

## Architecture

```
                 kernel: SO_REUSEPORT group hashes each new connection
                 to one listener
                         │              │              │
         ┌───────────────▼──┐  ┌────────▼─────────┐  ┌─▼────────────────┐
         │ worker 0 (cpu 0) │  │ worker 1 (cpu 1) │  │ worker N (cpu N) │
         │ listen fd        │  │ listen fd        │  │ ...              │
         │ epoll (ET)       │  │ epoll (ET)       │  │                  │
         │ eventfd (stop)   │  │ eventfd (stop)   │  │                  │
         │ connections{}    │  │ connections{}    │  │                  │
         │ WorkerMetrics ◄──┼──┼── alignas(64) ───┼──┼─► /metrics scrape│
         └──────────────────┘  └──────────────────┘  └──────────────────┘

 per connection:  recv ─► ByteBuffer ─► RequestParser ─► Router/handler
                                                              │
      socket ◄─ send(out) ◄─ sendfile(file) ◄─ producer(pull) ◄┘
```

Each connection runs a small loop, `drive()`, that repeats **read → parse/dispatch → write** until nothing makes progress:

- **Edge-triggered, registered once.** Each socket is added with `EPOLLIN|EPOLLOUT|EPOLLET` exactly once. The worker tracks `can_read`/`can_write` itself, so the hot path never calls `epoll_ctl(MOD)` to toggle interest in `EPOLLOUT`.
- **Responses stay in order under pipelining.** While a file or streamed body is being sent, later pipelined requests wait in the input buffer. Input is also capped, so a client can't pipeline unboundedly.
- **Framing is decided in one place.** `write_head()` derives `Content-Length`, `Transfer-Encoding` and `Connection` from the response object, so a handler can't advertise a length that doesn't match the body.

## The parser

[`RequestParser`](src/http/request_parser.hpp) is a push-style state machine: `RequestLine → Headers → Body | ChunkSize → ChunkData → ChunkDataEnd → Trailers → Done`. `feed()` consumes only complete units and returns how many bytes it used. The caller keeps the rest, which is exactly how a TCP stream has to be handled. The parser also stops at a request boundary, so pipelined requests stay in the buffer.

The tests feed every message at **every chunk size from 1 byte to the whole message**, because TCP can split a request at any byte boundary. The fuzzer adds a differential check on top: for any input, parsing it whole, one byte at a time, or in fuzzer-chosen chunk sizes must give *identical* results.

## Build and run

Linux only (it uses epoll, sendfile, SO_REUSEPORT and openat2). On Windows, use WSL2.

```bash
sudo apt install g++ make            # plus clang/cmake for the optional targets
make                                 # build/httpd, build/loadgen, build/httpd_tests
make test                            # 52 unit + end-to-end tests
make asan                            # AddressSanitizer + UndefinedBehaviorSanitizer
make tsan                            # ThreadSanitizer
make fuzz FUZZ_ITERS=1000000         # differential fuzzing (or: make fuzz-libfuzzer)
make bench                           # start the server and benchmark it with loadgen
./build/httpd --help
```

CMake works too: `cmake -S . -B build/cmake && cmake --build build/cmake && ctest --test-dir build/cmake`.

### Endpoints

| Endpoint | Purpose |
|---|---|
| `GET /healthz` | liveness probe |
| `GET /metrics` | Prometheus metrics |
| `GET /debug/info` | pid, workers, CPUs, page size, kernel, compiler, limits (JSON) |
| `POST /echo` | echoes the body; accepts Content-Length or chunked |
| `GET /stream/{n}` | `n` chunks with `X-Chunk-Count` / `X-Content-CRC32` trailers |
| `GET /bytes/{n}` | `n` generated bytes, streamed with backpressure |
| `GET /static/{path}` | files under `--root`, sent with `sendfile(2)` |

```bash
curl --raw localhost:8080/stream/3      # shows the chunk framing and trailers
curl localhost:8080/metrics | grep httpd_requests_total
```

## Benchmarks

Measured with the bundled `loadgen`, both processes on one machine over loopback: an i7-11800H (8C/16T) under WSL2 with kernel 6.18. The server had 4 workers, loadgen 4 threads and 128 keep-alive connections, each run lasting 5 s. Every run finished with **0 errors**.

| Workload | Throughput | p50 | p99 | p99.9 |
|---|---|---|---|---|
| `/healthz`, 1 request in flight per connection | **463k req/s** | 259 µs | 568 µs | 986 µs |
| `/healthz`, pipelined ×16 | **2.64M req/s** | 700 µs | 1.69 ms | 1.91 ms |
| `/bytes/65536` (streamed, backpressure path) | 60k req/s = **3.7 GiB/s** | 2.10 ms | 4.21 ms | 7.40 ms |
| `/static/index.html` (sendfile) | 191k req/s | 639 µs | 1.18 ms | 1.70 ms |
| `/healthz`, **1 worker** (same load) | 84k req/s | 1.48 ms | 2.42 ms | 3.10 ms |

The server-side `/metrics` from the same run show `SO_REUSEPORT` balance: 129/130/120/134 connections and 4.23M/4.20M/4.17M/4.16M requests across the four workers. They also show about 14 epoll events handled per `epoll_wait` wakeup under load.

Caveats: loopback only, the load generator competes with the server for the same cores, and WSL2 adds virtualization overhead. These numbers compare configurations; they are not absolute claims. `./scripts/bench.sh` reproduces them.

## Observability

```
httpd_connections_active{worker="0"} 12
httpd_requests_total{worker="0"} 4228255
httpd_eventloop_wakeups_total{worker="0"} 81808
httpd_eventloop_events_total{worker="0"} 1101804
httpd_bytes_sendfile_total 452369958
httpd_responses_total{code="2xx"} 16752220
httpd_request_duration_seconds_bucket{le="0.000512"} ...
```

- **No contention on the counters.** Each worker is the *only writer* of its own `WorkerMetrics` block, so increments are a relaxed load plus store (plain `mov`s) rather than a `lock`-prefixed `fetch_add`. `alignas(64)` keeps workers from false-sharing cache lines.
- **Fixed-size latency histogram.** It uses power-of-two µs buckets computed with `__builtin_clzll`, needs no allocation, and aggregates across workers when scraped.
- **Easier debugging.** Worker threads are named `httpd-w<N>`, so they show up by name in `top -H`, `perf` and `gdb`.

## Design notes and bugs handled on purpose

- **`sendfile` and SIGPIPE.** `send()` can take `MSG_NOSIGNAL`, but `sendfile()` has no such flag. Without `SIGPIPE` ignored, a client disconnecting mid-download would kill the whole process.
- **Lingering close.** Calling `close()` with unread input makes the kernel send an RST, which can wipe out a `400` response before the client reads it. The server does `shutdown(SHUT_WR)`, drains input, then closes.
- **Partial-line limits must be split-independent.** An unterminated line of exactly `limit+1` bytes may end in the `\r` of the CRLF, so the partial-line check uses `> limit + 1`. Otherwise the same request would be accepted or rejected depending on where TCP split it, which is precisely what the differential fuzzer checks for.
- **Signals are handled synchronously.** SIGINT/SIGTERM are blocked in every thread and received with `sigwait()` in `main`, so shutdown code doesn't have to be async-signal-safe. Workers are woken through an `eventfd`.
- **TSan on newer kernels.** `vm.mmap_rnd_bits=32` breaks TSan's shadow-memory layout ("unexpected memory mapping"). `make tsan` runs the tests under `setarch -R`.

## Layout

```
src/http/      request parser, headers, response serialization, chunked coding
src/server/    epoll workers, connection state machine, router
src/obs/       metrics + Prometheus exposition
src/app/       routes, static file server (openat2 + sendfile)
src/util/      UniqueFd (RAII fd), ByteBuffer, CRC-32
tests/         self-registering test framework, parser/response/e2e tests
fuzz/          libFuzzer target + standalone mutator (differential)
tools/         loadgen
scripts/       bench.sh
```

## Roadmap

- `io_uring` backend behind the same connection state machine, to compare syscall counts against epoll
- A hierarchical timer wheel instead of the periodic O(n) timeout sweep
- kTLS (TLS record encryption in the kernel, which keeps `sendfile` zero-copy with TLS)
- IPv6 / dual-stack listeners, `SO_INCOMING_CPU`, busy polling experiments
- `perf`/eBPF flame graphs of the hot path checked into `docs/`

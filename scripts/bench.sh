#!/usr/bin/env bash
# Starts httpd, runs loadgen against a few endpoints, prints server-side
# metrics, and shuts the server down gracefully.
#
#   THREADS=4 CONNS=128 DURATION=10 ./scripts/bench.sh
set -euo pipefail

BUILD=${BUILD:-build}
PORT=${PORT:-18080}
THREADS=${THREADS:-$(nproc)}
CONNS=${CONNS:-128}
DURATION=${DURATION:-10}
LOADGEN_THREADS=${LOADGEN_THREADS:-$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))}

"$BUILD/httpd" --port "$PORT" --threads "$THREADS" --root public ${PIN:+--pin} &
SERVER=$!
trap 'kill -TERM $SERVER 2>/dev/null; wait $SERVER 2>/dev/null || true' EXIT
sleep 0.3

run() {
  echo
  echo "== $1 =="
  "$BUILD/loadgen" --port "$PORT" --path "$2" -c "$CONNS" -t "$LOADGEN_THREADS" -d "$DURATION" ${3:+-p $3}
}

echo "httpd: $THREADS worker(s), loadgen: $LOADGEN_THREADS thread(s), $CONNS connections, ${DURATION}s per run"
run "small response, keep-alive" /healthz
run "small response, pipelined x16" /healthz 16
run "64 KiB streamed body" /bytes/65536
run "static file via sendfile" /static/index.html

echo
echo "== server-side metrics =="
curl -s "http://127.0.0.1:$PORT/metrics" | grep -E '^httpd_(requests_total|connections_accepted_total|eventloop_(wakeups|events)_total|bytes_sendfile_total|request_duration_seconds_count)' || true

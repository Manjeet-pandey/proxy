#!/usr/bin/env bash
# Repeatable throughput benchmark: median of N rounds, no profiling overhead.
# Use this before/after a change so you know whether the profile actually helped.
#
#   scripts/bench.sh                 # 3 rounds x 10s, 64 connections
#   scripts/bench.sh -r 5 -t 30      # 5 rounds x 30s
#   scripts/bench.sh --delay-ms 5    # slow upstream (exercises more buffering)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
# Override to A/B a different build without touching CMake, e.g.
#   PROXY_BIN=/tmp/variant scripts/bench.sh
PROXY_BIN="${PROXY_BIN:-$BUILD_DIR/edge-proxy-asio}"
SERVER_PORT="${SERVER_PORT:-3000}"
PROXY_PORT="${PROXY_PORT:-9000}"
ROUNDS=3
DURATION=10
CONNS=64
DELAY_MS=0
DO_BUILD=1
KILL_STALE=0

die() { echo "error: $*" >&2; exit 1; }

usage() {
  cat <<'EOF'
usage: scripts/bench.sh [options]

  -r, --rounds N       measured rounds                (default 3)
  -t, --duration SECS  seconds per round              (default 10)
  -c, --conns N        concurrent connections         (default 64)
      --delay-ms N     backend artificial latency     (default 0)
      --kill-stale     stop leftover test binaries on the ports
      --no-build       skip the cmake build
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -r|--rounds)   ROUNDS="$2"; shift 2 ;;
    -t|--duration) DURATION="$2"; shift 2 ;;
    -c|--conns)    CONNS="$2"; shift 2 ;;
    --delay-ms)    DELAY_MS="$2"; shift 2 ;;
    --kill-stale)  KILL_STALE=1; shift ;;
    --no-build)    DO_BUILD=0; shift ;;
    -h|--help)     usage; exit 0 ;;
    *)             die "unknown option: $1 (try --help)" ;;
  esac
done

command -v oha >/dev/null || die "oha not found (brew install oha)"
grep -q "api.example.com" /etc/hosts || die "/etc/hosts needs: 127.0.0.1  api.example.com"

SERVER_PID=""
PROXY_PID=""
cleanup() {
  local pid
  for pid in "$PROXY_PID" "$SERVER_PID"; do
    [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
  done
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# NB: lsof exits 1 when nothing matches; keep that out of `set -e`.
listeners() { lsof -nP -iTCP:"$1" -sTCP:LISTEN -t 2>/dev/null | tr '\n' ' ' || true; }
stale_ok() {
  local comm
  comm="$(ps -o comm= -p "$1" 2>/dev/null)"
  comm="${comm##*/}"
  case "$comm" in
    server|edge-proxy-asio|edge-prox*) return 0 ;;
    *) return 1 ;;
  esac
}
free_up_port() {
  local port="$1" pids pid
  pids="$(listeners "$port")"
  if [[ -z "${pids// /}" ]]; then return 0; fi
  if [[ "$KILL_STALE" -eq 1 ]]; then
    for pid in $pids; do
      stale_ok "$pid" || die "port $port held by pid $pid ($(ps -o comm= -p "$pid")); stop it yourself"
      kill "$pid" 2>/dev/null || true
    done
    for _ in $(seq 1 50); do
      if [[ -z "$(listeners "$port")" ]]; then return 0; fi
      sleep 0.1
    done
    die "port $port still busy"
  fi
  die "port $port is already in use (pids: $pids); use --kill-stale or stop them"
}
wait_port() {
  local port="$1" pid="$2"
  for _ in $(seq 1 100); do
    if [[ -n "$(listeners "$port")" ]]; then return 0; fi
    kill -0 "$pid" 2>/dev/null || die "$(ps -o comm= -p "$pid") exited early"
    sleep 0.1
  done
  die "timed out waiting for port $port"
}

if [[ "$DO_BUILD" -eq 1 ]]; then
  cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
  cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)" 2>&1 | tail -3
fi

free_up_port "$SERVER_PORT"
free_up_port "$PROXY_PORT"

"$BUILD_DIR/server" "$SERVER_PORT" --delay-ms "$DELAY_MS" >/dev/null 2>&1 &
SERVER_PID=$!
wait_port "$SERVER_PORT" "$SERVER_PID"
"$PROXY_BIN" --port "$PROXY_PORT" --upstream "127.0.0.1:$SERVER_PORT" >/dev/null 2>&1 &
PROXY_PID=$!
wait_port "$PROXY_PORT" "$PROXY_PID"

URL="http://api.example.com:$PROXY_PORT/health"
oha -n 2000 -c 10 --no-tui "$URL" >/dev/null 2>&1 || die "warmup failed"

echo "load: $CONNS connections, ${DURATION}s per round, $ROUNDS round(s)"
printf '%-7s %-12s %-10s %-10s %-8s\n' round req/sec p50 p99 success
rps_values=()
for r in $(seq 1 "$ROUNDS"); do
  oha -c "$CONNS" -z "${DURATION}s" --no-tui "$URL" >/tmp/bench.$$ 2>&1 || true
  rps=$(awk '/Requests\/sec:/{print $2}' /tmp/bench.$$)
  p50=$(awk '/ 50\.00% in /{print $3}' /tmp/bench.$$)
  p99=$(awk '/ 99\.00% in /{print $3}' /tmp/bench.$$)
  succ=$(awk -F'\t' '/Success rate:/{print $2}' /tmp/bench.$$ | tr -d ' ')
  rps_values+=("$rps")
  printf '%-7s %-12s %-10s %-10s %-8s\n' "$r" "$rps" "${p50}ms" "${p99}ms" "$succ"
done
rm -f /tmp/bench.$$

median=$(printf '%s\n' "${rps_values[@]}" | sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}')
best=$(printf '%s\n' "${rps_values[@]}" | sort -n | tail -1)
echo
echo "median: $median req/sec   (best $best)"

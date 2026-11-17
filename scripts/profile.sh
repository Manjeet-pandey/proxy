#!/usr/bin/env bash
# Profile the edge-proxy under load with macOS sample(1).
#
#   scripts/profile.sh                       # build, load, sample the proxy
#   scripts/profile.sh --target both         # sample proxy + backend
#   scripts/profile.sh --conns 200 -t 30     # heavier / longer run
#   scripts/profile.sh --reuse               # sample already-running instances
#
# Results land in prof/: raw sample output, a readable report, folded stacks
# (for flamegraphs) and the oha load summary.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-$ROOT/build-prof}"
OUT_DIR="${OUT_DIR:-$ROOT/prof}"
PROXY_BIN="${PROXY_BIN:-$BUILD_DIR/edge-proxy-asio}"
SERVER_PORT="${SERVER_PORT:-3000}"
PROXY_PORT="${PROXY_PORT:-9000}"
CONNS=64
DURATION=20
SAMPLE_SECS=12
DELAY_MS=0
TARGET=proxy
DO_BUILD=1
KILL_STALE=0
REUSE=0

die() { echo "error: $*" >&2; exit 1; }

usage() {
  cat <<'EOF'
usage: scripts/profile.sh [options]

  -c, --conns N        concurrent load connections         (default 64)
  -t, --duration SECS  how long to drive load              (default 20)
  -s, --sample SECS    how long to sample                  (default 12)
      --delay-ms N     backend artificial latency          (default 0)
      --target WHAT    proxy | server | both               (default proxy)
      --reuse          do not start anything; sample the
                       processes already on the ports
      --kill-stale     stop leftover build/server and
                       build/edge-proxy-asio holding the ports
      --no-build       skip the cmake build
      --out DIR        output directory                    (default ./prof)

Everything is time-boxed; the script cleans up what it started.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--conns)      CONNS="$2"; shift 2 ;;
    -t|--duration)   DURATION="$2"; shift 2 ;;
    -s|--sample)     SAMPLE_SECS="$2"; shift 2 ;;
    --delay-ms)      DELAY_MS="$2"; shift 2 ;;
    --target)        TARGET="$2"; shift 2 ;;
    --reuse)         REUSE=1; shift ;;
    --kill-stale)    KILL_STALE=1; shift ;;
    --no-build)      DO_BUILD=0; shift ;;
    --out)           OUT_DIR="$2"; shift 2 ;;
    -h|--help)       usage; exit 0 ;;
    *)               die "unknown option: $1 (try --help)" ;;
  esac
done

case "$TARGET" in proxy|server|both) ;; *) die "--target must be proxy, server or both" ;; esac
[[ "$DURATION" -gt 3 ]] || die "--duration must be > 3s"
[[ "$SAMPLE_SECS" -lt "$DURATION" ]] || die "--sample must be less than --duration"

command -v oha >/dev/null || die "oha not found (brew install oha)"
command -v sample >/dev/null || die "sample(1) not found (part of Xcode CLT)"
grep -q "api.example.com" /etc/hosts \
  || die "/etc/hosts needs: 127.0.0.1  api.example.com"

SERVER_PID=""
PROXY_PID=""
LOAD_PID=""
# Only kill what this script started -- with --reuse the pids belong to you.
OWN_SERVER=0
OWN_PROXY=0

cleanup() {
  local pid
  [[ "$OWN_PROXY" -eq 1 && -n "$PROXY_PID" ]] && kill "$PROXY_PID" 2>/dev/null || true
  [[ "$OWN_SERVER" -eq 1 && -n "$SERVER_PID" ]] && kill "$SERVER_PID" 2>/dev/null || true
  [[ -n "$LOAD_PID" ]] && kill "$LOAD_PID" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# NB: lsof exits 1 when nothing matches; keep that out of `set -e`.
listeners() { lsof -nP -iTCP:"$1" -sTCP:LISTEN -t 2>/dev/null | tr '\n' ' ' || true; }

# Only ever kill processes that look like *our* test binaries.
stale_ok() {
  local pid="$1" comm
  comm="$(ps -o comm= -p "$pid" 2>/dev/null)"
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
      if stale_ok "$pid"; then
        echo "  stopping stale pid $pid on port $port"
        kill "$pid" 2>/dev/null || true
      else
        die "port $port is held by pid $pid ($(ps -o comm= -p "$pid")), not a test binary; stop it yourself"
      fi
    done
    for _ in $(seq 1 50); do
      if [[ -z "$(listeners "$port")" ]]; then return 0; fi
      sleep 0.1
    done
    die "port $port still busy after --kill-stale"
  fi
  die "port $port is already in use (pids: $pids).
       Stop those processes, or re-run with --kill-stale to stop our test binaries,
       or --reuse to profile what is already running."
}

wait_port() {
  local port="$1" pid="$2" i
  for i in $(seq 1 100); do
    if [[ -n "$(listeners "$port")" ]]; then return 0; fi
    kill -0 "$pid" 2>/dev/null || die "$(ps -o comm= -p "$pid") exited while starting (see $OUT_DIR/*.log)"
    sleep 0.1
  done
  die "timed out waiting for port $port"
}

listener_pid() { lsof -nP -iTCP:"$1" -sTCP:LISTEN -t 2>/dev/null | head -1; }

mkdir -p "$OUT_DIR"
# Old artifacts must not leak into this run's report.
rm -f "$OUT_DIR"/sample-*.txt "$OUT_DIR"/sample-*.stderr \
      "$OUT_DIR"/report*.txt "$OUT_DIR"/*.folded "$OUT_DIR"/load.txt
if [[ "$DO_BUILD" -eq 1 ]]; then
  echo "==> building $BUILD_DIR (RelWithDebInfo + frame pointers)"
  cmake -S "$ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEDGE_PROXY_PROFILE=ON >/dev/null
  cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)" 2>&1 | tail -5
fi
[[ -x "$PROXY_BIN" ]] || die "no binary at $PROXY_BIN (run without --no-build)"
[[ -x "$BUILD_DIR/server" ]] || die "no binary at $BUILD_DIR/server (run without --no-build)"

echo "==> checking ports"
if [[ "$REUSE" -eq 1 ]]; then
  PROXY_PID="$(listener_pid "$PROXY_PORT")"
  SERVER_PID="$(listener_pid "$SERVER_PORT")"
  [[ -n "$PROXY_PID" ]] || die "--reuse: nothing listening on $PROXY_PORT"
  echo "  reusing proxy pid $PROXY_PID, backend pid ${SERVER_PID:-none}"
  echo "  (if that binary was not built with -g, symbols will be missing)"
else
  free_up_port "$SERVER_PORT"
  free_up_port "$PROXY_PORT"

  local_delay=""
  if [[ "$DELAY_MS" -ne 0 ]]; then local_delay=" (delay ${DELAY_MS}ms)"; fi
  echo "==> starting backend on :$SERVER_PORT$local_delay"
  "$BUILD_DIR/server" "$SERVER_PORT" --delay-ms "$DELAY_MS" >"$OUT_DIR/server.log" 2>&1 &
  SERVER_PID=$!
  OWN_SERVER=1
  wait_port "$SERVER_PORT" "$SERVER_PID"

  echo "==> starting proxy on :$PROXY_PORT"
  "$PROXY_BIN" --port "$PROXY_PORT" --upstream "127.0.0.1:$SERVER_PORT" >"$OUT_DIR/proxy.log" 2>&1 &
  PROXY_PID=$!
  OWN_PROXY=1
  wait_port "$PROXY_PORT" "$PROXY_PID"
  PROXY_PID="$(listener_pid "$PROXY_PORT")"
fi

URL="http://api.example.com:$PROXY_PORT/health"

echo "==> warmup"
oha -n 3000 -c 10 --no-tui "$URL" >/dev/null 2>&1 || die "warmup failed; check $OUT_DIR/proxy.log"

echo "==> load: oha -c $CONNS -z ${DURATION}s"
oha -c "$CONNS" -z "${DURATION}s" --no-tui "$URL" >"$OUT_DIR/load.txt" 2>&1 &
LOAD_PID=$!
sleep 2

sample_pid() {
  local name="$1" pid="$2" out="$OUT_DIR/sample-$1.txt"
  echo "==> sampling $name (pid $pid) for ${SAMPLE_SECS}s"
  if ! sample "$pid" "$SAMPLE_SECS" -f "$out" >"$OUT_DIR/sample-$name.stderr" 2>&1; then
    echo "  sample failed for $name:" >&2
    tail -3 "$OUT_DIR/sample-$name.stderr" >&2
    return 1
  fi
}

case "$TARGET" in
  proxy)  sample_pid proxy  "$PROXY_PID" ;;
  server) [[ -n "$SERVER_PID" ]] || die "no backend process to sample on :$SERVER_PORT"
          sample_pid server "$SERVER_PID" ;;
  both)
          [[ -n "$SERVER_PID" ]] || die "no backend process to sample on :$SERVER_PORT"
          # Wait on the samplers only -- a bare `wait` would also block on the
          # long-running backend/proxy children.
          sample_pid proxy  "$PROXY_PID" & SP=$!
          sample_pid server "$SERVER_PID" & SS=$!
          wait "$SP" || true
          wait "$SS" || true ;;
esac

wait "$LOAD_PID" || true
LOAD_PID=""

{
  echo "date:      $(date)"
  echo "commit:    $(git rev-parse --short HEAD 2>/dev/null || echo n/a)"
  echo "build:     $BUILD_DIR (RelWithDebInfo, EDGE_PROXY_PROFILE=ON)"
  echo "conns:     $CONNS"
  echo "duration:  ${DURATION}s, sampled ${SAMPLE_SECS}s"
  echo "target:    $TARGET"
  echo "host:      $(uname -m) $(sw_vers -productVersion), $(sysctl -n hw.ncpu) cpus"
  echo "flags:     $(sed -n 's/^CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=//p' "$BUILD_DIR/CMakeCache.txt")" \
       "EDGE_PROXY_PROFILE=$(sed -n 's/^EDGE_PROXY_PROFILE:BOOL=//p' "$BUILD_DIR/CMakeCache.txt")"
} >"$OUT_DIR/meta.txt"

echo
echo "=========== load summary ==========="
sed -n '/Summary:/,/Requests\/sec/p' "$OUT_DIR/load.txt"
grep -E "99\.00%|99\.90%" "$OUT_DIR/load.txt" | sed 's/^/  /' || true

if [[ -s "$OUT_DIR/sample-proxy.txt" ]]; then
  python3 "$ROOT/tools/prof.py" "$OUT_DIR/sample-proxy.txt" \
      --top 18 --folded "$OUT_DIR/proxy.folded" | tee "$OUT_DIR/report.txt"
fi
if [[ -s "$OUT_DIR/sample-server.txt" ]]; then
  python3 "$ROOT/tools/prof.py" "$OUT_DIR/sample-server.txt" \
      --top 12 --folded "$OUT_DIR/server.folded" >"$OUT_DIR/report-server.txt"
  echo "  backend report: $OUT_DIR/report-server.txt"
fi

echo
echo "artifacts in $OUT_DIR:"
echo "  report.txt          readable hot-function report"
echo "  sample-proxy.txt    raw sample(1) call tree"
echo "  proxy.folded        folded stacks (flamegraph / speedscope)"
echo "  meta.txt            build + load settings"

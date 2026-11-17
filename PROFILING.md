# Profiling edge-proxy

Two scripts and a report reader. macOS-native (no `perf`), no rebuild rituals,
no leftover processes.

```sh
scripts/bench.sh                 # how fast is it right now? (median of 3 rounds)
scripts/profile.sh               # what is it spending the time on?
```

Both start a `server` backend on `:3000`, the proxy on `:9000`, drive load with
`oha`, and clean up after themselves. `api.example.com` must resolve to
127.0.0.1 in `/etc/hosts`.

## Benchmark before and after

```sh
scripts/bench.sh -r 5 -t 15 -c 64
```

```
round   req/sec      p50        p99        success
1       60959.4233   1.0669ms   1.1854ms   100.00%
...
median: 62576.6081 req/sec   (best 62778.0499)
```

Use the **median**, not a single run. Keep `-c` fixed when comparing changes:
throughput is a function of both the proxy and the client's concurrency.

## Profile

```sh
scripts/profile.sh                      # 64 conns, 20 s load, 12 s sample
scripts/profile.sh -c 200 -t 20 -s 12   # saturated case, where hotspots matter
scripts/profile.sh --target both        # proxy + backend in one run
scripts/profile.sh --reuse              # sample instances you already started
```

It builds `build-prof/` with `CMAKE_BUILD_TYPE=RelWithDebInfo` and
`-DEDGE_PROXY_PROFILE=ON` (`-O2 -g -fno-omit-frame-pointer`), then runs
`sample(1)` against the live process while load is in flight.

Keep `-O2`. A `Debug`/`-O0` build changes the thing you are measuring: inlining
and the optimizer are exactly what you want in the picture.

### What lands in `prof/`

| file | what it is |
| --- | --- |
| `report.txt` | the readable answer: hot functions, per-thread split |
| `sample-proxy.txt` | raw `sample(1)` call tree (indented, template-heavy) |
| `proxy.folded` | folded stacks, one line per stack |
| `load.txt` | full `oha` output: rps, percentiles, errors |
| `meta.txt` | commit, flags, concurrency, host |
| `sample-server.txt`, `report-server.txt` | only with `--target both` |

### Reading `report.txt`

- **per-thread samples / "working thread(s)"** — the first thing to check on a
  multi-threaded server. If one thread owns ~100% of the runnable samples, more
  threads are not helping, and the reason is not in the profile.
- **time split** — `runnable` vs `blocked/idle`. An async server that is 95%
  blocked in `kevent` is queueing, not computing; a profile taken there shows
  you latency behaviour, not CPU cost. Profile with enough concurrency that
  `runnable` is high (see `--conns`).
- **SELF time** — where the CPU actually is.
- **INCLUSIVE time** — which call path to attack: the share of samples whose
  stack contains that frame (deduped per stack, so it never exceeds 100%).
- **syscall / kernel time** — for a proxy this is usually the headline.

Rule of thumb: if `average latency ≈ conns / rps`, the server is saturated and
the latency you are looking at is just queueing. More connections will raise
latency and leave throughput flat.

Idle waiter threads (`main` blocked in `thread::join`) are excluded from the
percentages automatically; otherwise they halve every number.

## Flame graphs

`sample` does not emit folded stacks, so `tools/prof.py` does:

```sh
# visual, no install: drag prof/proxy.folded onto https://www.speedscope.app
# or, locally:
brew install flamegraph          # provides flamegraph.pl
flamegraph.pl --title edge-proxy prof/proxy.folded > prof/proxy.svg
```

For interactive timeline/pane views, Instruments still works:

```sh
xctrace record --template 'Time Profiler' --launch -- ./build-prof/edge-proxy-asio
open *.trace
```

## Gotchas

- `sample` needs the process to stay alive for the whole window; the script
  keeps load running longer than the sample for that reason.
- Symbol quality depends on `-g` and frame pointers. Both are on in
  `build-prof/`; a `-O0` or stripped build gives you addresses.
- Sampling costs throughput (it suspends the target at 1 kHz). A sampled run
  measured ~32–58k rps where the same binary does ~63k unsampled. Use
  `bench.sh` for numbers, `profile.sh` for shape.
- Port `:3000`/`:9000` conflicts abort the run on purpose. `--kill-stale` only
  stops `server`/`edge-proxy-asio` processes, never other programs.
- `oha -t` is a *timeout*, not a duration; the scripts use `-z` for duration.

## What the first profile showed (2026-10-22, 12-core arm64)

Measured with the current `src/async_proxy.cpp`:

```
samples: 7180 from 1 working thread(s)
  100.0% of all   7180 samples   98.1% busy  Thread_...
time split:  98.1% runnable,   1.9% blocked/idle

  55.7%  __sendmsg
  18.8%  __recvfrom
   1.9%  kevent
```

1. **Only one worker thread exists at runtime.** `main()` spawns four threads
   and each builds its own `io_context` and calls `listener()`, which opens and
   binds port 9000. The first thread wins the bind; the other three fail and
   their `run()` returns, so they exit silently. `ps` confirms one thread at
   ~99% CPU. That single thread is the throughput ceiling: ~63k rps at 64
   connections, flat as connections increase.

2. **The serving thread spends ~75% of its CPU in two syscalls**
   (`__sendmsg` 56%, `__recvfrom` 19%) — one read and one write per hop, four
   per request. That is close to the syscall floor for a "read → parse → write"
   proxy on macOS loopback, so there is little left to win by micro-tuning the
   hot path; the win is running the existing work on more cores.

The fix was a code change, not a profiling one: `main()` now opens a single
acceptor on one shared `io_context` and runs it on all worker threads. A clean
re-run of `bench.sh` and `profile.sh -c 200` to confirm the curve moved is
still to do.

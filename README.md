# edge-proxy

A small HTTP/1.1 reverse proxy in C++20, built on Boost.Asio coroutines and
Boost.Beast. I wrote it to get hands-on with C++ coroutines and to see how
close a straightforward async design gets to the syscall floor on a single
machine, with a repeatable benchmark and profiling setup around it.

## How it works

- One acceptor, one shared `io_context`, run by N worker threads.
- Each client connection gets its own coroutine (`handle_session`) that reads
  a request, forwards it, writes the response back, and loops while the client
  keeps the connection alive. Pipelined bytes stay in the per-connection buffer.
- Each session opens its own keep-alive connection to the upstream on first use
  and reuses it for later requests on that client connection.
- Requests whose `Host` (port stripped) does not match the configured name get
  `421 Misdirected Request`; a missing `Host` gets `400`.
- If the upstream cannot be reached, or fails mid-exchange, the client gets
  `502 Bad Gateway` and the connection is closed.
- Request bodies over 1 MB get `413 Payload Too Large`.
- Accept errors are logged and the listener keeps going; when out of file
  descriptors it backs off for 100 ms before retrying.

The code is in `src/async_proxy.cpp`. `src/server.cpp` is a small Asio test
backend used by the benchmark scripts.

## Build

Needs CMake 3.16+, a C++20 compiler and Boost (headers only). On macOS:

```sh
brew install boost cmake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This produces `build/edge-proxy-asio` (the proxy) and `build/server` (test backend).

## Run

```sh
./build/server 3000 &
./build/edge-proxy-asio --port 9000 --upstream 127.0.0.1:3000 --host api.example.com
curl -H 'Host: api.example.com' localhost:9000/
```

| flag | default |
| --- | --- |
| `--port N` | 9000 |
| `--upstream IP:PORT` | 127.0.0.1:3000 |
| `--host NAME` | api.example.com |
| `--threads N` | number of CPUs |

## Benchmark

`scripts/bench.sh` builds, starts the backend and the proxy, drives load with
[oha](https://github.com/hatoo/oha) and reports the median of several rounds.
It needs `127.0.0.1 api.example.com` in `/etc/hosts`.

```sh
scripts/bench.sh -r 5 -t 15 -c 64
```

Apple M4 Pro (12 cores), loopback, 64 connections, 5 rounds x 15 s, proxy,
backend and oha on the same machine:

| median req/s | best req/s | p50 | p99 |
| --- | --- | --- | --- |
| 62,577 | 62,778 | ~1.07 ms | ~1.19 ms |

This run was taken while the proxy was still effectively single-threaded (see
the profiling notes); it has not been re-measured cleanly since the switch to a
shared `io_context`. Profiling setup and findings are in
[PROFILING.md](PROFILING.md).

## Limitations

- Bodies are fully buffered in memory (`string_body`), not streamed. Requests
  are capped at 1 MB; responses at Beast's default of 8 MB (larger ones become
  a 502).
- A single upstream address, no load balancing or health checks, no
  hostname resolution for `--upstream`.
- If a reused upstream connection was closed by the backend between requests,
  that request fails with 502 rather than being retried.
- HTTP/1.1 only, no TLS, no timeouts on idle or slow clients, no graceful
  shutdown.
- Only tested on macOS; the scripts use macOS tools (`sysctl`, `sample`).

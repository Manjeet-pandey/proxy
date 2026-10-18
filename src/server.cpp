// Test backend for the edge-proxy prototype.
//
// Replaces the old serial `accept -> recv -> send -> close` loop, which could
// only ever handle one connection at a time and used a listen backlog of 5.
// Under a 400-connection load that produced mass connection resets that the
// proxy dutifully reported as 502s -- the backend, not the proxy, was the
// bottleneck.
//
// This version is multi-threaded, one coroutine per connection, and speaks
// enough HTTP/1.1 to keep connections alive, so the proxy's own connection
// accounting is what gets measured. Deliberately does no per-request logging:
// writing to std::cerr from the io_context threads is what made the proxy
// collapse under load, and it would do the same here.
//
// Usage: ./server [port] [threads] [--delay-ms N] [--stats]
//   port     default 3000
//   threads  default: hardware concurrency
//   --delay-ms  artificial per-request latency, to exercise slow-upstream paths
//   --stats      print a throughput line every 5s (off by default)

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using boost::asio::ip::tcp;

namespace {

// The proxy's Day 2 success signal is a reply of exactly this, so the body is
// part of the test contract -- don't change it. Note it is 17 bytes, not the 16
// the old server advertised; that mismatch desynced keep-alive framing.
constexpr const char *kBody = "Hey from server!\n";

constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr std::size_t kReadChunk = 8192;
constexpr std::chrono::seconds kStatsInterval{5};

struct Options {
  std::uint16_t port = 3000;
  unsigned threads = 0; // 0 -> hardware concurrency
  std::chrono::milliseconds delay{0};
  bool stats = false;
};

struct Counters {
  std::atomic<std::uint64_t> connections{0};
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> bad_requests{0};
  std::atomic<std::uint64_t> bytes_out{0};
  std::atomic<int> active{0};
  std::atomic<int> peak_active{0};
};

std::string make_response(bool keep_alive) {
  std::string r;
  r.reserve(160);
  r += "HTTP/1.1 200 OK\r\n";
  r += "Content-Type: text/plain\r\n";
  r += "Content-Length: " + std::to_string(std::strlen(kBody)) + "\r\n";
  r += "Connection: " + std::string(keep_alive ? "keep-alive" : "close") +
       "\r\n";
  r += "\r\n";
  r += kBody;
  return r;
}

std::string make_error_response(const char *code, const char *reason) {
  std::string r;
  r += "HTTP/1.1 ";
  r += code;
  r += ' ';
  r += reason;
  r += "\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n"
       "Connection: close\r\n\r\n";
  return r;
}

// Case-insensitive search for `name` at the start of a header line, and the
// offset of its value. Returns std::string::npos when absent.
std::size_t find_header(std::string_view headers, std::string_view name,
                        std::size_t &value_start) {
  std::size_t pos = 0;
  while (pos < headers.size()) {
    const std::size_t line_start = pos;
    std::size_t eol = headers.find("\r\n", pos);
    if (eol == std::string_view::npos)
      eol = headers.size();
    std::string_view line = headers.substr(line_start, eol - line_start);
    pos = eol + 2;

    if (line.size() <= name.size())
      continue;
    if (line.size() > name.size() && line[name.size()] != ':')
      continue;

    bool match = true;
    for (std::size_t i = 0; i < name.size(); ++i) {
      char a = line[i];
      char b = name[i];
      if (a >= 'A' && a <= 'Z')
        a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z')
        b = static_cast<char>(b - 'A' + 'a');
      if (a != b) {
        match = false;
        break;
      }
    }
    if (!match)
      continue;

    std::size_t v = name.size() + 1;
    while (v < line.size() && (line[v] == ' ' || line[v] == '\t'))
      ++v;
    value_start = line_start + v; // absolute, into `headers`
    return pos;
  }
  return std::string_view::npos;
}

std::string_view header_value(std::string_view headers, std::string_view name) {
  std::size_t vs = 0;
  std::size_t body = find_header(headers, name, vs);
  if (body == std::string_view::npos)
    return {};
  std::size_t eol = headers.find("\r\n", vs);
  if (eol == std::string_view::npos)
    eol = headers.size();
  return headers.substr(vs, eol - vs);
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  return std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
    if (x >= 'A' && x <= 'Z')
      x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z')
      y = static_cast<char>(y - 'A' + 'a');
    return x == y;
  });
}

struct RequestInfo {
  bool keep_alive = false;
  std::size_t body_bytes = 0;
  bool well_formed = false;
};

RequestInfo parse_request(std::string_view req) {
  RequestInfo info;

  std::size_t sp1 = req.find(' ');
  if (sp1 == std::string_view::npos)
    return info;
  std::size_t sp2 = req.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos)
    return info;
  std::size_t vend = req.find_first_of("\r\n", sp2 + 1);
  if (vend == std::string_view::npos)
    vend = req.size();
  std::string_view version = req.substr(sp2 + 1, vend - sp2 - 1);
  if (version != "HTTP/1.0" && version != "HTTP/1.1")
    return info;

  std::size_t head_end = req.find("\r\n\r\n");
  if (head_end == std::string_view::npos)
    return info;

  std::string_view headers = req.substr(0, head_end + 2);

  // Chunked bodies aren't supported by this test rig; refusing them is the
  // only framing-safe answer, since we can't tell where they end.
  if (!header_value(headers, "Transfer-Encoding").empty()) {
    info.keep_alive = false;
    info.well_formed = true; // parsed, but force close
    return info;
  }

  std::string_view cl = header_value(headers, "Content-Length");
  if (!cl.empty()) {
    unsigned long long n = 0;
    bool ok = !cl.empty();
    for (char ch : cl) {
      if (ch < '0' || ch > '9') {
        ok = false;
        break;
      }
      n = n * 10 + static_cast<unsigned>(ch - '0');
      if (n > kMaxRequestBytes) {
        ok = false;
        break;
      }
    }
    if (!ok)
      return info; // malformed -> 400
    info.body_bytes = static_cast<std::size_t>(n);
  }

  bool http11 = version == "HTTP/1.1";

  // "Connection" is a comma-separated token list, e.g. "keep-alive, Upgrade".
  std::string_view conn = header_value(headers, "Connection");
  bool conn_close = false;
  bool conn_keep = false;
  std::size_t start = 0;
  while (start <= conn.size()) {
    std::size_t comma = conn.find(',', start);
    std::string_view tok = comma == std::string_view::npos
                               ? conn.substr(start)
                               : conn.substr(start, comma - start);
    if (iequals(tok, "close"))
      conn_close = true;
    if (iequals(tok, "keep-alive"))
      conn_keep = true;
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }

  info.keep_alive = conn_close ? false : (http11 ? true : conn_keep);
  info.well_formed = true;
  return info;
}

// Reads until one complete request is buffered. Returns false on EOF, error,
// or an over-long request line.
boost::asio::awaitable<bool> read_request(tcp::socket &sock, std::string &buf,
                                          std::size_t &header_len) {
  char chunk[kReadChunk];
  for (;;) {
    std::size_t end = buf.find("\r\n\r\n");
    if (end != std::string::npos) {
      header_len = end + 4;
      co_return true;
    }
    if (buf.size() > kMaxRequestBytes)
      co_return false;

    boost::system::error_code ec;
    std::size_t n = co_await sock.async_read_some(
        boost::asio::buffer(chunk),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    if (ec || n == 0)
      co_return false;
    buf.append(chunk, n);
  }
}

boost::asio::awaitable<void> handle_session(tcp::socket sock, Options opt,
                                            Counters &counters) {
  counters.connections.fetch_add(1, std::memory_order_relaxed);
  int active = counters.active.fetch_add(1, std::memory_order_relaxed) + 1;
  int prev = counters.peak_active.load(std::memory_order_relaxed);
  while (active > prev && !counters.peak_active.compare_exchange_weak(
                              prev, active, std::memory_order_relaxed)) {
  }

  // Replies are small and the body never changes, so hand the kernel a single
  // contiguous buffer instead of a scatter/gather list.
  const std::string keep_alive_response = make_response(true);
  const std::string close_response = make_response(false);

  std::string buf;
  buf.reserve(kReadChunk * 2);

  for (;;) {
    std::size_t header_len = 0;
    if (!co_await read_request(sock, buf, header_len))
      break;

    RequestInfo info =
        parse_request(std::string_view(buf).substr(0, header_len));

    if (!info.well_formed) {
      counters.bad_requests.fetch_add(1, std::memory_order_relaxed);
      const std::string err = make_error_response("400", "Bad Request");
      boost::system::error_code ig;
      boost::asio::write(sock, boost::asio::buffer(err), ig);
      break;
    }

    // Drain a declared body so keep-alive framing stays in sync. Anything past
    // the cap was already rejected in parse_request().
    bool peer_gone = false;
    while (buf.size() - header_len < info.body_bytes) {
      std::size_t want = info.body_bytes - (buf.size() - header_len);
      char chunk[kReadChunk];
      boost::system::error_code ec;
      std::size_t n = co_await sock.async_read_some(
          boost::asio::buffer(chunk, want < kReadChunk ? want : kReadChunk),
          boost::asio::redirect_error(boost::asio::use_awaitable, ec));
      if (ec || n == 0) {
        peer_gone = true;
        break;
      }
      buf.append(chunk, n);
    }
    if (peer_gone)
      break; // still falls through to the active-counter decrement
    buf.erase(0, header_len + info.body_bytes);

    if (opt.delay.count() > 0)
      co_await boost::asio::steady_timer(sock.get_executor(), opt.delay)
          .async_wait(boost::asio::use_awaitable);

    const std::string &response =
        info.keep_alive ? keep_alive_response : close_response;
    boost::system::error_code ec;
    co_await boost::asio::async_write(
        sock, boost::asio::buffer(response),
        boost::asio::redirect_error(boost::asio::use_awaitable, ec));
    if (ec)
      break;

    counters.requests.fetch_add(1, std::memory_order_relaxed);
    counters.bytes_out.fetch_add(response.size(), std::memory_order_relaxed);

    if (!info.keep_alive)
      break;
  }

  counters.active.fetch_sub(1, std::memory_order_relaxed);
}

boost::asio::awaitable<void> run_listener(tcp::acceptor &acceptor, Options opt,
                                          Counters &counters) {
  for (;;) {
    boost::system::error_code ec;
    tcp::socket sock = co_await acceptor.async_accept(
        boost::asio::redirect_error(boost::asio::use_awaitable, ec));

    if (ec) {
      if (ec == boost::asio::error::operation_aborted)
        co_return;
      // Never let a transient accept error kill the listener. The old code let
      // this throw out of the coroutine, which -- with a detached completion
      // handler -- silently destroyed the acceptor and let io.run() return,
      // taking the whole server down mid-benchmark.
      std::cerr << "[server] accept failed: " << ec.message()
                << " (retrying)\n";
      co_await boost::asio::steady_timer(acceptor.get_executor(),
                                         std::chrono::milliseconds(20))
          .async_wait(boost::asio::use_awaitable);
      continue;
    }

    boost::system::error_code ig;
    sock.set_option(tcp::no_delay(true), ig);

    boost::asio::co_spawn(sock.get_executor(),
                          handle_session(std::move(sock), opt, counters),
                          boost::asio::detached);
  }
}

boost::asio::awaitable<void> run_stats(Counters &counters,
                                       boost::asio::any_io_executor ex) {
  std::uint64_t last_requests = 0;
  auto last = std::chrono::steady_clock::now();
  for (;;) {
    co_await boost::asio::steady_timer(ex, kStatsInterval)
        .async_wait(boost::asio::use_awaitable);
    auto now = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(now - last).count();
    std::uint64_t reqs = counters.requests.load(std::memory_order_relaxed);
    std::cout << "[server] "
              << (static_cast<double>(reqs - last_requests) / secs)
              << " req/s  conns=" << counters.connections.load()
              << "  active=" << counters.active.load()
              << "  peak=" << counters.peak_active.load() << "  bytes_out="
              << counters.bytes_out.load(std::memory_order_relaxed)
              << std::endl;
    last_requests = reqs;
    last = now;
  }
}

void print_usage(const char *argv0) {
  std::cerr << "usage: " << argv0
            << " [port] [threads] [--delay-ms N] [--stats]\n";
}

Options parse_args(int argc, char **argv) {
  Options opt;
  int i = 1;
  if (i < argc && argv[i][0] != '-')
    opt.port = static_cast<std::uint16_t>(std::atoi(argv[i++]));
  if (i < argc && argv[i][0] != '-')
    opt.threads = static_cast<unsigned>(std::atoi(argv[i++]));

  for (; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--stats") {
      opt.stats = true;
    } else if (arg == "--delay-ms" && i + 1 < argc) {
      opt.delay = std::chrono::milliseconds(std::atoi(argv[++i]));
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      print_usage(argv[0]);
      std::exit(2);
    }
  }

  if (opt.threads == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    opt.threads = hw ? hw : 4;
  }
  return opt;
}

} // namespace

int main(int argc, char **argv) {
  Options opt = parse_args(argc, argv);

  boost::asio::io_context io;
  Counters counters;

  // Explicit open/bind/listen with error_code overloads, so a port clash (the
  // usual cause is a stale server still holding it) reports one readable line
  // instead of an uncaught exception dumping a Boost template blob.
  boost::system::error_code ec;
  tcp::acceptor acceptor(io);
  acceptor.open(tcp::v4(), ec);
  if (!ec)
    acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
  if (!ec)
    acceptor.bind(tcp::endpoint(tcp::v4(), opt.port), ec);
  if (!ec)
    acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) {
    std::cerr << "[server] cannot listen on port " << opt.port << ": "
              << ec.message() << "\n";
    if (ec == boost::asio::error::address_in_use)
      std::cerr << "[server] another process is already on that port -- stop "
                   "it or pass a different one\n";
    return 1;
  }

  boost::asio::co_spawn(io, run_listener(acceptor, opt, counters),
                        boost::asio::detached);
  if (opt.stats)
    boost::asio::co_spawn(io, run_stats(counters, io.get_executor()),
                          boost::asio::detached);

  boost::asio::signal_set signals(io, SIGINT, SIGTERM);
  std::signal(SIGPIPE, SIG_IGN);
  boost::asio::co_spawn(
      io,
      [&signals, &counters, &io]() -> boost::asio::awaitable<void> {
        boost::system::error_code sec;
        co_await signals.async_wait(
            boost::asio::redirect_error(boost::asio::use_awaitable, sec));
        std::cout << "\n[server] shutting down: conns="
                  << counters.connections.load()
                  << " requests=" << counters.requests.load() << "\n";
        io.stop();
      },
      boost::asio::detached);

  std::vector<std::thread> pool;
  pool.reserve(opt.threads);
  for (unsigned i = 0; i < opt.threads; ++i)
    pool.emplace_back([&io] { io.run(); });
  for (auto &t : pool)
    t.join();

  return 0;
}

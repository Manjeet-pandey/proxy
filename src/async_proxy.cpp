// HTTP/1.1 reverse proxy: one coroutine per client connection, each with its
// own keep-alive connection to a single upstream.

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;

namespace {

struct Config {
  unsigned short port = 9000;
  std::string host = "api.example.com"; // only Host value we accept
  tcp::endpoint upstream{boost::asio::ip::make_address("127.0.0.1"), 3000};
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
};

Config cfg; // set once in main() before any thread starts

const std::string kCanned502 = "HTTP/1.1 502 Bad Gateway\r\n"
                               "Content-Type: text/plain\r\n"
                               "Content-Length: 12\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Bad Gateway\n";

http::response<http::string_body> make_error(http::status s) {
  http::response<http::string_body> r{s, 11};
  r.set(http::field::content_type, "text/plain");
  r.body() = std::string(http::obsolete_reason(s)) + "\n";
  r.keep_alive(false);
  r.prepare_payload();
  return r;
}

boost::asio::awaitable<void> handle_session(tcp::socket client) {
  tcp::socket upstream(client.get_executor());
  bool upstream_alive = false;

  boost::system::error_code ec;
  client.set_option(tcp::no_delay(true), ec); // not inherited from acceptor

  beast::flat_buffer client_buf; // per connection: keeps pipelined bytes
  beast::flat_buffer upstream_buf;

  auto token = boost::asio::redirect_error(boost::asio::use_awaitable, ec);

  try {
    for (;;) {
      // ---------- request ----------
      http::request_parser<http::string_body> req_p; // 1 MB body limit
      co_await http::async_read(client, client_buf, req_p, token);
      if (ec == http::error::body_limit) {
        auto err = make_error(http::status::payload_too_large);
        co_await http::async_write(client, err, token);
        co_return;
      }
      if (ec)
        co_return; // client closed or sent garbage
      auto &req = req_p.get();

      std::string host = req[http::field::host];
      if (auto p = host.find(':'); p != std::string::npos)
        host = host.substr(0, p);
      if (host != cfg.host) {
        auto err = make_error(host.empty() ? http::status::bad_request
                                           : http::status::misdirected_request);
        co_await http::async_write(client, err, token);
        co_return;
      }

      bool client_ka = req.keep_alive(); // capture BEFORE rewriting headers
      bool is_head = req.method() == http::verb::head;
      req.erase(http::field::proxy_connection);
      req.keep_alive(true); // upstream conn is ours, keep it

      // ---------- upstream ----------
      if (!upstream_alive) {
        co_await upstream.async_connect(cfg.upstream,
                                        boost::asio::use_awaitable); // -> 502
        upstream.set_option(tcp::no_delay(true), ec);
        upstream_alive = true;
      }

      co_await http::async_write(upstream, req, token);
      if (ec)
        throw beast::system_error(ec); // -> 502

      http::response_parser<http::string_body> res_p;
      res_p.skip(is_head); // HEAD: Content-Length, no body
      co_await http::async_read(upstream, upstream_buf, res_p, token);
      if (ec)
        throw beast::system_error(ec); // stale/dead upstream -> 502
      auto &res = res_p.get();

      // ---------- response ----------
      bool upstream_ka = res.keep_alive();
      bool client_close = !client_ka || res_p.need_eof(); // EOF-delimited body
      res.keep_alive(!client_close);

      co_await http::async_write(client, res, token);
      if (ec)
        co_return; // client gone

      if (!upstream_ka) { // upstream closing != client closing
        upstream.close(ec);
        upstream_alive = false;
      }
      if (client_close)
        break;
    }
    client.shutdown(tcp::socket::shutdown_send, ec);

  } catch (const boost::system::system_error &e) {
    // only upstream failures reach here, and nothing was sent to the client yet
    std::cerr << "[session] upstream: " << e.code().message() << std::endl;
    boost::system::error_code ignore;
    boost::asio::write(client, boost::asio::buffer(kCanned502), ignore);
    client.shutdown(tcp::socket::shutdown_send, ignore);
  }
}

boost::asio::awaitable<void> listener(tcp::acceptor &acceptor) {
  auto ex = co_await boost::asio::this_coro::executor;
  boost::asio::steady_timer backoff(ex);
  boost::system::error_code ec;
  auto token = boost::asio::redirect_error(boost::asio::use_awaitable, ec);

  for (;;) {
    tcp::socket sock = co_await acceptor.async_accept(token);
    if (ec) {
      std::cerr << "[listener] accept: " << ec.message() << std::endl;
      // Out of file descriptors: retrying immediately just spins. Give
      // in-flight sessions a moment to close theirs.
      if (ec == boost::asio::error::no_descriptors ||
          ec == boost::system::errc::too_many_files_open_in_system) {
        backoff.expires_after(std::chrono::milliseconds(100));
        co_await backoff.async_wait(token);
      }
      continue;
    }
    boost::asio::co_spawn(ex, handle_session(std::move(sock)),
                          boost::asio::detached);
  }
}

void usage(const char *argv0) {
  std::cerr << "usage: " << argv0
            << " [--port N] [--upstream IP:PORT] [--host NAME] [--threads N]\n"
               "defaults: --port 9000 --upstream 127.0.0.1:3000"
               " --host api.example.com --threads <cpu count>\n";
}

bool parse_args(int argc, char **argv) {
  try {
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "-h" || arg == "--help") {
        usage(argv[0]);
        std::exit(0);
      }
      if (i + 1 >= argc)
        return false;
      std::string val = argv[++i];
      if (arg == "--port") {
        cfg.port = static_cast<unsigned short>(std::stoul(val));
      } else if (arg == "--host") {
        cfg.host = val;
      } else if (arg == "--threads") {
        cfg.threads = std::max(1u, static_cast<unsigned>(std::stoul(val)));
      } else if (arg == "--upstream") {
        auto colon = val.rfind(':');
        if (colon == std::string::npos)
          return false;
        cfg.upstream = tcp::endpoint(
            boost::asio::ip::make_address(val.substr(0, colon)),
            static_cast<unsigned short>(std::stoul(val.substr(colon + 1))));
      } else {
        return false;
      }
    }
  } catch (const std::exception &) { // bad number or IP address
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (!parse_args(argc, argv)) {
    usage(argv[0]);
    return 2;
  }

  boost::asio::io_context io;

  boost::system::error_code ec;
  tcp::acceptor acceptor(io);
  acceptor.open(tcp::v4(), ec);
  if (!ec)
    acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
  if (!ec)
    acceptor.bind(tcp::endpoint(tcp::v4(), cfg.port), ec);
  if (!ec)
    acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) {
    std::cerr << "cannot listen on port " << cfg.port << ": " << ec.message()
              << std::endl;
    return 1;
  }

  std::cerr << "listening on :" << cfg.port << ", forwarding Host "
            << cfg.host << " to " << cfg.upstream << " with " << cfg.threads
            << " thread(s)" << std::endl;

  boost::asio::co_spawn(io, listener(acceptor), boost::asio::detached);

  std::vector<std::thread> pool;
  for (unsigned i = 0; i < cfg.threads; ++i)
    pool.emplace_back([&io] { io.run(); });
  for (auto &t : pool)
    t.join();

  return 0;
}

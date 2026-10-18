#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <iostream>
using boost::asio::ip::tcp;
namespace beast = boost::beast;

static const std::string KCanned502 = "HTTP/1.1 502 Bad Gateway\r\n"
                                      "Content-Type: text/plain\r\n"
                                      "Content-Length: 12\r\n"
                                      "Connection: close\r\n"
                                      "\r\n"
                                      "Bad Gateway\n";

/*
 *
 * ❯ oha -n 2000 -c 50 -t 2s --no-tui --disable-keepalive
http://api.example.com:9000 Summary: Success rate: 100.00% Total: 142.5270 ms
  Slowest:      8.6609 ms
  Fastest:      2.1810 ms
  Average:      3.4810 ms
  Requests/sec: 14032.4289

  Total data:   33.20 KiB
  Size/request: 17 B
  Size/sec:     232.96 KiB

Response time histogram:
  2.181 ms [1]    |
  2.829 ms [84]   |■
  3.477 ms [1696] |■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■■
  4.125 ms [19]   |
  4.773 ms [1]    |
  5.421 ms [7]    |
  6.069 ms [16]   |
  6.717 ms [18]   |
  7.365 ms [22]   |
  8.013 ms [23]   |
  8.661 ms [113]  |■■

Response time distribution:
  10.00% in 2.8831 ms
  25.00% in 2.9395 ms
  50.00% in 3.0109 ms
  75.00% in 3.1242 ms
  90.00% in 4.7391 ms
  95.00% in 8.2186 ms
  99.00% in 8.5618 ms
  99.90% in 8.6370 ms
  99.99% in 8.6609 ms


Details (average, fastest, slowest):
  DNS+dialup:   0.9787 ms, 0.5045 ms, 2.9080 ms
  DNS-lookup:   0.0037 ms, 0.0007 ms, 0.5117 ms

Status code distribution:
  [200] 2000 responses
 *
 *
 *
 *
 *
 *
 *
 *
 *
 * */

// boost::asio::awaitable<void> read_message(tcp::socket &sock, std::string
// &buf) {
//   boost::system::error_code ec;
//   while (true) {
//     auto header_end = buf.find("\r\n\r\n");
//     if(header_end != std::string::npos){
//       std::size_t header_len = header_end + 4;
//     }
//     std::size_t n = co_await sock.async_read_some(
//         boost::asio::buffer(buf),
//         boost::asio::redirect_error(boost::asio::use_awaitable, ec));
//   }
// }

boost::asio::awaitable<void> handle_session(tcp::socket sock) {
  tcp::socket client(std::move(sock));
  tcp::socket upstream(client.get_executor());
  bool upstream_alive = false;
  try {
    for (;;) {

      boost::system::error_code ec;
      beast::flat_buffer client_buf;
      beast::http::request<beast::http::string_body> req;

      co_await beast::http::async_read(
          client, client_buf, req,
          boost::asio::redirect_error(boost::asio::use_awaitable, ec));

      if (ec) {
        if (ec == beast::http::error::end_of_stream ||
            ec == boost::asio::error::eof)
          co_return;
        throw beast::system_error(ec);
      }

      std::string host = req[beast::http::field::host];
      if (host.empty()) {
        co_return;
      }
      if (auto p = host.find(':'); p != std::string::npos)
        host = host.substr(0, p);

      if (host != "api.example.com") {
        co_return;
      }
      if (!upstream_alive) {
        co_await upstream.async_connect(
            tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 3000),
            boost::asio::use_awaitable);
        upstream_alive = true;
      }

      co_await beast::http::async_write(upstream, req,
                                        boost::asio::use_awaitable);

      beast::http::response<beast::http::string_body> res;
      beast::flat_buffer upstream_buf;
      co_await beast::http::async_read(
          upstream, upstream_buf, res,
          boost::asio::redirect_error(boost::asio::use_awaitable, ec));

      if (ec) {
        if (ec == beast::http::error::end_of_stream ||
            ec == boost::asio::error::eof)
          co_return;
        throw beast::system_error(ec);
      }
      bool close = !req.keep_alive() || !res.keep_alive();
      if (close) {
        res.keep_alive(false);
      }

      co_await beast::http::async_write(client, res,
                                        boost::asio::use_awaitable);
      if (close) {
        upstream_alive = false;
        break;
      }
    }
    boost::system::error_code ec;
    client.shutdown(tcp::socket::shutdown_send, ec);

  } catch (const std::exception &e) {
    std::cerr << "[session] FATAL: " << e.what() << std::endl;
    boost::system::error_code ignore;
    boost::asio::write(client, boost::asio::buffer(KCanned502), ignore);
    client.shutdown(tcp::socket::shutdown_send, ignore);
  }
}
boost::asio::awaitable<void> listener(int listener_port) {
  auto ex = co_await boost::asio::this_coro::executor;
  tcp::acceptor acceptor_(ex);
  boost::system::error_code ec;

  if (acceptor_.open(tcp::v4(), ec)) {
    co_return;
  }

  if (acceptor_.set_option(tcp::acceptor::reuse_address(true), ec)) {
    co_return;
  }

  if (acceptor_.bind(tcp::endpoint(tcp::v4(), listener_port), ec)) {
    co_return;
  }

  if (acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec)) {
    co_return;
  }

  while (true) {
    tcp::socket sock = co_await acceptor_.async_accept();
    boost::asio::co_spawn(ex, handle_session(std::move(sock)),
                          boost::asio::detached);
  }

  co_return;
}

int main() {
  boost::asio::io_context io;
  int listener_port = 9000;
  boost::asio::co_spawn(io, listener(listener_port), boost::asio::detached);
  std::vector<std::thread> pools;
  pools.reserve(4);
  for (unsigned i = 0; i < 4; i++) {
    pools.emplace_back([&io] { io.run(); });
  }

  for (auto &t : pools) {
    t.join();
  }

  return 0;
}

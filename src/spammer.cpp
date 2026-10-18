// Sequential HTTP client for the edge proxy.
//
// Not a load generator: this issues ONE request at a time, on a fresh
// connection, and reads the complete response before starting the next one.
// Every exchange is printed so you can see exactly what the proxy sent back.
//
// The proxy routes on the Host header and closes the connection without
// replying when it does not match, so --host-header defaults to the value
// the proxy actually expects.
//
// Usage: spammer [host] [port] [count] [path] [host-header]
//   ./spammer 127.0.0.1 9000 5
//   ./spammer 127.0.0.1 9000 20 /health api.example.com

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int kRecvTimeoutSec = 5;

struct Outcome {
  bool connected = false;
  bool sent = false;
  int status = 0;      // parsed HTTP status code, 0 if none
  long long bytes = 0; // total response bytes read
  double millis = 0.0;
  std::string error;   // non-empty on any failure
};

// Case-insensitive search for `name` at the start of a header line. Returns
// std::string_view of the trimmed value, or an empty view when absent.
std::string_view header_value(std::string_view headers, std::string_view name) {
  while (!headers.empty()) {
    auto eol = headers.find("\r\n");
    if (eol == std::string_view::npos) return {};
    auto line = headers.substr(0, eol);
    headers.remove_prefix(eol + 2);

    if (line.size() > name.size() + 1 && line[name.size()] == ':') {
      bool match = true;
      for (std::size_t i = 0; i < name.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(line[i])) !=
            std::tolower(static_cast<unsigned char>(name[i]))) {
          match = false;
          break;
        }
      }
      if (match) {
        auto v = line.substr(name.size() + 1);
        auto a = v.find_first_not_of(" \t");
        if (a == std::string_view::npos) return {};
        auto b = v.find_last_not_of(" \t");
        return v.substr(a, b - a + 1);
      }
    }
  }
  return {};
}

// Pull the status code out of the status line. 0 when the line is malformed.
int parse_status(std::string_view response) {
  if (response.compare(0, 5, "HTTP/") != 0) return 0;
  auto sp = response.find(' ');
  if (sp == std::string_view::npos) return 0;
  int status = 0;
  auto i = sp + 1;
  while (i < response.size() && std::isdigit(static_cast<unsigned char>(
                                      response[i]))) {
    status = status * 10 + (response[i] - '0');
    i++;
  }
  return status;
}

void die(const std::string &msg) {
  std::cerr << "spammer: " << msg << "\n";
  std::exit(1);
}

// One request, one connection, one complete response.
Outcome exchange(const std::string &ip, int port, const std::string &path,
                 const std::string &host_header) {
  Outcome out;
  auto start = std::chrono::steady_clock::now();

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    out.error = std::string("socket: ") + std::strerror(errno);
    return out;
  }

  timeval tv{kRecvTimeoutSec, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    out.error = "bad IP '" + ip + "' (use a numeric address like 127.0.0.1)";
    close(fd);
    return out;
  }

  if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    out.error = std::string("connect: ") + std::strerror(errno);
    close(fd);
    return out;
  }
  out.connected = true;

  // One request per connection: ask for close so the proxy's shutdown
  // send is a clean end of stream rather than a truncated body.
  std::string req = "GET " + path + " HTTP/1.1\r\n";
  req += "Host: " + host_header + "\r\n";
  req += "User-Agent: spammer\r\n";
  req += "Connection: close\r\n";
  req += "\r\n";

  std::size_t sent = 0;
  while (sent < req.size()) {
    ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
    if (n <= 0) {
      out.error = std::string("send: ") + std::strerror(errno);
      close(fd);
      return out;
    }
    sent += static_cast<std::size_t>(n);
  }
  out.sent = true;

  // Read until we have the whole body, then stop. Falling back to EOF covers
  // responses without a Content-Length.
  std::string resp;
  char buf[4096];
  std::size_t header_end = std::string::npos;
  long long content_length = -1;

  while (true) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      out.error = std::string("recv: ") + std::strerror(errno);
      break;
    }
    if (n == 0) break; // clean EOF from the proxy's shutdown
    resp.append(buf, static_cast<std::size_t>(n));

    if (header_end == std::string::npos) {
      header_end = resp.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        header_end += 4;
        std::string_view headers(resp.data(), header_end);
        auto cl = header_value(headers, "Content-Length");
        if (!cl.empty()) {
          content_length = std::strtoll(std::string(cl).c_str(), nullptr, 10);
        }
        if (header_value(headers, "Transfer-Encoding").find("chunked") !=
            std::string_view::npos) {
          content_length = -1; // don't try to frame a chunked body
        }
      }
    }

    if (content_length >= 0 && header_end != std::string::npos) {
      auto have = static_cast<long long>(resp.size() - header_end);
      if (have >= content_length) break; // body complete
    }
  }

  close(fd);
  out.millis = std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start)
                   .count();
  out.bytes = static_cast<long long>(resp.size());

  if (!out.error.empty()) return out;
  if (resp.empty()) {
    out.error = "empty response (proxy closed without replying)";
    return out;
  }
  if (header_end == std::string::npos) {
    out.error = "no header terminator in response";
    return out;
  }
  if (content_length >= 0 &&
      static_cast<long long>(resp.size() - header_end) < content_length) {
    out.error = "truncated body: got " +
                std::to_string(resp.size() - header_end) + " of " +
                std::to_string(content_length) + " bytes";
    return out;
  }

  out.status = parse_status(resp);
  if (out.status == 0) out.error = "unparseable status line";
  return out;
}

} // namespace

int main(int argc, char **argv) {
  std::string ip = "127.0.0.1";
  int port = 9000;
  int count = 1;
  std::string path = "/";
  std::string host_header = "api.example.com";

  if (argc > 1) ip = argv[1];
  if (argc > 2) port = std::atoi(argv[2]);
  if (argc > 3) count = std::atoi(argv[3]);
  if (argc > 4) path = argv[4];
  if (argc > 5) host_header = argv[5];

  if (port < 1 || port > 65535) die("port must be 1-65535");
  if (count < 1) die("count must be >= 1");

  std::cout << "GET http://" << ip << ":" << port << path << "  x" << count
            << "  (Host: " << host_header << ")\n\n";

  std::map<int, int> by_status;
  int failed = 0;
  double total_ms = 0.0;
  std::vector<double> latencies;
  latencies.reserve(static_cast<std::size_t>(count));

  for (int i = 1; i <= count; i++) {
    Outcome r = exchange(ip, port, path, host_header);
    latencies.push_back(r.millis);
    total_ms += r.millis;

    std::cout << "[" << i << "/" << count << "] ";
    if (!r.error.empty()) {
      failed++;
      std::cout << "FAIL  " << r.millis << "ms  " << r.error << "\n\n";
      continue;
    }

    by_status[r.status]++;
    std::cout << (r.status >= 200 && r.status < 400 ? "ok    " : "http  ")
              << r.status << "  " << r.millis << "ms  " << r.bytes
              << " bytes\n";
  }

  std::cout << "\n---\n" << count << " request(s), " << failed
            << " failed\n";
  if (!by_status.empty()) {
    for (auto [status, n] : by_status) {
      std::cout << "  " << status << ": " << n << "\n";
    }
  }
  if (!latencies.empty()) {
    auto sorted = latencies;
    std::sort(sorted.begin(), sorted.end());
    std::cout << "  avg:   " << (total_ms / static_cast<double>(count))
              << "ms\n"
              << "  min:   " << sorted.front() << "ms\n"
              << "  p50:   " << sorted[sorted.size() / 2] << "ms\n"
              << "  max:   " << sorted.back() << "ms\n";
  }

  return failed > 0 ? 1 : 0;
}

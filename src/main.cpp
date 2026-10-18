#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/_endian.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
  const int listen_port = 9000;

  // Create socket
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Cannot create socket: " << strerror(errno) << std::endl;
    return 1;
  }

  // bind port to socket
  sockaddr_in address{};
  address.sin_port = htons(listen_port);
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;

  if (bind(server_fd, reinterpret_cast<sockaddr *>(&address),
           sizeof(address))) {
    std::cerr << "Cannot bind port " << listen_port << ": " << strerror(errno)
              << std::endl;
    close(server_fd);
    return 1;
  }
  // listen on port; 5 means no of active clients
  listen(server_fd, 5);
  std::cout << "Listening on port " << listen_port << "...\n";

  // accept loop
  while (true) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(
        server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd < 0) {
      std::cerr << "Accept failed: " << strerror(errno) << std::endl;
      continue;
    }
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    std::cout << "Client connected form " << client_ip << " : "
              << ntohs(client_addr.sin_port) << std::endl;

    sockaddr_in local_addr{};
    socklen_t local_len = sizeof(local_addr);
    getsockname(client_fd, reinterpret_cast<sockaddr *>(&local_addr),
                &local_len);
    char local_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local_addr.sin_addr, local_ip, sizeof(local_ip));

    std::cout << " Client " << client_ip << ":" << ntohs(client_addr.sin_port)
              << " connected to " << local_ip << ":"
              << ntohs(local_addr.sin_port) << std::endl;

    char buf[4096];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
      close(client_fd);
      return 1;
    }
    buf[n] = '\0';

    std::cout << "=== RAW REQUEST (" << n << " bytes) ===" << std::endl;
    std::cout << buf << std::endl;
    std::cout << "=== END ===" << std::endl;

    const char *response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: 19\r\n"
                           "Connection: close\r\n"
                           "\r\n"
                           "Hello from server!\n";
    send(client_fd, response, strlen(response), 0);
  }

  close(server_fd);
  return 0;
}

#include "track/http_post.hpp"

#include <cerrno>
#include <cstring>
#include <format>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace hermes::track {

namespace {

struct Fd {
  int fd = -1;
  ~Fd() {
    if (fd >= 0) ::close(fd);
  }
};

bool send_all(int fd, std::string_view data) {
  while (!data.empty()) {
#ifdef MSG_NOSIGNAL
    const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
#else
    const ssize_t n = ::send(fd, data.data(), data.size(), 0);
#endif
    if (n <= 0) {
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
    data.remove_prefix(static_cast<std::size_t>(n));
  }
  return true;
}

}  // namespace

bool http_post(std::string_view url, std::string_view path, std::string_view body, std::string_view token,
               double timeout_s, std::string* err) {
  auto fail = [&](std::string m) {
    if (err) *err = std::move(m);
    return false;
  };
  if (url.rfind("https://", 0) == 0) return fail("https is not supported (use http over a tunnel or VPN)");
  if (url.rfind("http://", 0) == 0) url.remove_prefix(7);
  std::string_view hostport = url, prefix;
  if (const auto p = url.find('/'); p != std::string_view::npos) {
    hostport = url.substr(0, p);
    prefix = url.substr(p);
    while (!prefix.empty() && prefix.back() == '/') prefix.remove_suffix(1);
  }
  std::string host(hostport), port = "80";
  if (const auto c = hostport.rfind(':'); c != std::string_view::npos && hostport.find(']') == std::string_view::npos) {
    host = std::string(hostport.substr(0, c));
    port = std::string(hostport.substr(c + 1));
  }
  if (host.empty()) return fail("no host in server url");

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) {
    return fail(std::format("cannot resolve {}: {}", host, gai_strerror(rc)));
  }
  Fd s;
  timeval tv{};
  tv.tv_sec = static_cast<long>(timeout_s);
  tv.tv_usec = static_cast<long>((timeout_s - static_cast<double>(tv.tv_sec)) * 1e6);
  for (addrinfo* a = res; a; a = a->ai_next) {
    s.fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (s.fd < 0) continue;
    ::setsockopt(s.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    ::setsockopt(s.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(s.fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (::connect(s.fd, a->ai_addr, a->ai_addrlen) == 0) break;
    ::close(s.fd);
    s.fd = -1;
  }
  ::freeaddrinfo(res);
  if (s.fd < 0) return fail(std::format("cannot connect to {}:{}: {}", host, port, std::strerror(errno)));

  std::string req = std::format(
      "POST {}{} HTTP/1.1\r\nHost: {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n"
      "Connection: close\r\n",
      prefix, path, hostport, body.size());
  if (!token.empty()) req += std::format("X-Hermes-Token: {}\r\n", token);
  req += "\r\n";
  if (!send_all(s.fd, req) || !send_all(s.fd, body)) return fail("send failed");
  char buf[512];
  std::string head;
  while (head.find("\r\n") == std::string::npos) {
    const ssize_t n = ::recv(s.fd, buf, sizeof buf, 0);
    if (n <= 0) break;
    head.append(buf, static_cast<std::size_t>(n));
  }
  // "HTTP/1.1 200 OK"
  const auto sp = head.find(' ');
  if (sp == std::string::npos || head.size() < sp + 4) return fail("no HTTP answer");
  const int code = std::atoi(head.c_str() + sp + 1);
  if (code < 200 || code >= 300) return fail(std::format("server answered {}", head.substr(0, head.find("\r\n"))));
  return true;
}

}  // namespace hermes::track

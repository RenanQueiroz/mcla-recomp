#include "online_http.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "online_util.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kBadSocket = INVALID_SOCKET;
static int LastSocketError() { return WSAGetLastError(); }
static void CloseSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
static constexpr socket_t kBadSocket = -1;
static int LastSocketError() { return errno; }
static void CloseSocket(socket_t s) { close(s); }
#endif

namespace mcla::online {

void HttpStartup() {
#ifdef _WIN32
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
#endif
}

uint32_t ResolveIpv4(const std::string& host) {
  HttpStartup();
  uint32_t literal = ParseIpv4(host);
  if (literal) {
    return literal;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
    return 0;
  }
  uint32_t ip = 0;
  for (addrinfo* a = result; a; a = a->ai_next) {
    if (a->ai_family == AF_INET) {
      ip = ntohl(reinterpret_cast<sockaddr_in*>(a->ai_addr)->sin_addr.s_addr);
      break;
    }
  }
  freeaddrinfo(result);
  return ip;
}

uint32_t LocalAddressTowards(const std::string& host, uint16_t port) {
  HttpStartup();
  uint32_t ip = ResolveIpv4(host);
  if (!ip) {
    return 0;
  }
  socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBadSocket) {
    return 0;
  }
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(port ? port : 9);
  to.sin_addr.s_addr = htonl(ip);
  uint32_t local = 0;
  // A UDP connect sends nothing; it only picks the route.
  if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
    sockaddr_in me{};
    socklen_t len = sizeof(me);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
      local = ntohl(me.sin_addr.s_addr);
    }
  }
  CloseSocket(s);
  return local;
}

namespace {

void SetNonBlocking(socket_t s, bool on) {
#ifdef _WIN32
  u_long mode = on ? 1 : 0;
  ioctlsocket(s, FIONBIO, &mode);
#else
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

bool WaitFor(socket_t s, bool for_write, uint32_t timeout_ms) {
  fd_set set;
  FD_ZERO(&set);
  FD_SET(s, &set);
  timeval tv;
  tv.tv_sec = long(timeout_ms / 1000);
  tv.tv_usec = long((timeout_ms % 1000) * 1000);
  int r = select(int(s + 1), for_write ? nullptr : &set, for_write ? &set : nullptr, nullptr, &tv);
  return r > 0;
}

bool ConnectWithTimeout(socket_t s, const sockaddr_in& addr, uint32_t timeout_ms,
                        std::string* error) {
  SetNonBlocking(s, true);
  int r = connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (r != 0) {
    int e = LastSocketError();
#ifdef _WIN32
    bool in_progress = e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    bool in_progress = e == EINPROGRESS || e == EWOULDBLOCK;
#endif
    if (!in_progress) {
      *error = "connect failed (" + std::to_string(e) + ")";
      return false;
    }
    if (!WaitFor(s, true, timeout_ms)) {
      *error = "connect timed out";
      return false;
    }
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
    if (so_error != 0) {
      *error = "connect refused (" + std::to_string(so_error) + ")";
      return false;
    }
  }
  SetNonBlocking(s, false);
  return true;
}

bool SendAll(socket_t s, const std::string& data, uint32_t timeout_ms) {
  size_t sent = 0;
  while (sent < data.size()) {
    if (!WaitFor(s, true, timeout_ms)) {
      return false;
    }
    int n = send(s, data.data() + sent, int(data.size() - sent), 0);
    if (n <= 0) {
      return false;
    }
    sent += size_t(n);
  }
  return true;
}

}  // namespace

HttpResponse HttpRequest(const std::string& method, const std::string& host, uint16_t port,
                         const std::string& path, const std::string& body,
                         uint32_t timeout_ms) {
  HttpStartup();
  HttpResponse response;

  uint32_t ip = ResolveIpv4(host);
  if (!ip) {
    response.error = "could not resolve " + host;
    return response;
  }

  socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == kBadSocket) {
    response.error = "socket() failed";
    return response;
  }
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(ip);
  if (!ConnectWithTimeout(s, addr, timeout_ms, &response.error)) {
    CloseSocket(s);
    return response;
  }

  std::string request = method + " " + path + " HTTP/1.1\r\n";
  request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
  request += "User-Agent: mcla-recomp-online/1\r\n";
  request += "Accept: application/json\r\n";
  request += "Connection: close\r\n";
  if (!body.empty()) {
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  } else if (method != "GET") {
    request += "Content-Length: 0\r\n";
  }
  request += "\r\n";
  request += body;

  if (!SendAll(s, request, timeout_ms)) {
    response.error = "send failed";
    CloseSocket(s);
    return response;
  }

  // Read until the server closes, the Content-Length is satisfied, or time
  // runs out. The server answers with Connection: close, so this is enough.
  std::string raw;
  size_t header_end = std::string::npos;
  size_t content_length = std::string::npos;
  const uint64_t deadline = NowMs() + timeout_ms;
  char buf[8192];
  while (true) {
    uint64_t now = NowMs();
    if (now >= deadline) {
      response.error = "response timed out";
      break;
    }
    if (!WaitFor(s, false, uint32_t(deadline - now))) {
      response.error = "response timed out";
      break;
    }
    int n = recv(s, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;  // closed
    }
    raw.append(buf, size_t(n));
    if (header_end == std::string::npos) {
      header_end = raw.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        std::string headers = Lower(raw.substr(0, header_end));
        size_t cl = headers.find("content-length:");
        if (cl != std::string::npos) {
          content_length = size_t(std::atoll(headers.c_str() + cl + 15));
        }
      }
    }
    if (header_end != std::string::npos && content_length != std::string::npos &&
        raw.size() >= header_end + 4 + content_length) {
      break;
    }
  }
  CloseSocket(s);

  if (header_end == std::string::npos) {
    if (response.error.empty()) {
      response.error = "no HTTP response";
    }
    return response;
  }
  // "HTTP/1.1 200 OK"
  size_t sp = raw.find(' ');
  if (sp != std::string::npos && sp + 4 <= raw.size()) {
    response.status = std::atoi(raw.c_str() + sp + 1);
  }
  response.body = raw.substr(header_end + 4);
  if (content_length != std::string::npos && response.body.size() > content_length) {
    response.body.resize(content_length);
  }
  if (response.status == 0) {
    response.error = "bad status line";
  } else {
    response.error.clear();
  }
  return response;
}

}  // namespace mcla::online

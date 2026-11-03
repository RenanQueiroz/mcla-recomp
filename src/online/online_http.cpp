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
#include <windows.h>
#include <winhttp.h>
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

std::string HttpUrl::ToString() const {
  std::string s = https ? "https://" : "http://";
  s += host;
  if (port && port != (https ? 443 : 80)) {
    s += ":" + std::to_string(port);
  }
  s += base_path;
  return s;
}

bool ParseHttpUrl(std::string_view text, HttpUrl* out, uint16_t default_port) {
  std::string t = Trim(text);
  HttpUrl url;
  bool scheme_given = false;
  if (Lower(t.substr(0, 8)) == "https://") {
    url.https = true;
    scheme_given = true;
    t = t.substr(8);
  } else if (Lower(t.substr(0, 7)) == "http://") {
    scheme_given = true;
    t = t.substr(7);
  }
  // The path, if any.
  size_t slash = t.find('/');
  if (slash != std::string::npos) {
    url.base_path = t.substr(slash);
    t = t.substr(0, slash);
    while (!url.base_path.empty() && url.base_path.back() == '/') {
      url.base_path.pop_back();
    }
  }
  if (t.empty()) {
    return false;
  }
  size_t colon = t.rfind(':');
  if (colon != std::string::npos) {
    int p = std::atoi(t.c_str() + colon + 1);
    if (p <= 0 || p > 65535) {
      return false;
    }
    url.port = uint16_t(p);
    url.host = t.substr(0, colon);
  } else {
    url.host = t;
    url.port = scheme_given ? (url.https ? 443 : 80) : default_port;
  }
  if (url.host.empty()) {
    return false;
  }
  *out = url;
  return true;
}

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

#ifdef _WIN32

// --- WinHTTP ------------------------------------------------------------------------

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_SECURE_PROTOCOLS
#define WINHTTP_OPTION_SECURE_PROTOCOLS 84
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif

namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return std::wstring();
  }
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}

std::string WinHttpErrorText(DWORD error) {
  switch (error) {
    case 12002: return "timed out";
    case 12005: return "bad URL";
    case 12007: return "the name could not be resolved";
    case 12029: return "could not connect";
    case 12030: return "the connection was closed";
    case 12152: return "not an HTTP response";
    case 12157: return "the secure connection failed";
    case 12175: return "the certificate was not accepted";
    default: return "WinHTTP error " + std::to_string(error);
  }
}

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

}  // namespace

HttpResponse HttpRequest(const std::string& method, const HttpUrl& url, const std::string& path,
                         const std::string& body, const std::vector<std::string>& headers,
                         uint32_t timeout_ms) {
  HttpResponse response;
  Handle session;
  session.h = WinHttpOpen(L"mcla-recomp-online/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session.h) {
    session.h = WinHttpOpen(L"mcla-recomp-online/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session.h) {
    response.error = WinHttpErrorText(GetLastError());
    return response;
  }
  const int t = int(timeout_ms);
  WinHttpSetTimeouts(session.h, t, t, t, t);
  if (url.https) {
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols,
                          sizeof(protocols))) {
      protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
      WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
  }

  Handle connect;
  connect.h = WinHttpConnect(session.h, Widen(url.host).c_str(), url.port, 0);
  if (!connect.h) {
    response.error = WinHttpErrorText(GetLastError());
    return response;
  }

  Handle request;
  const std::wstring full_path = Widen(url.base_path + path);
  request.h = WinHttpOpenRequest(connect.h, Widen(method).c_str(), full_path.c_str(), nullptr,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 url.https ? WINHTTP_FLAG_SECURE : 0);
  if (!request.h) {
    response.error = WinHttpErrorText(GetLastError());
    return response;
  }

  std::string header_text = "Accept: application/json\r\n";
  if (!body.empty()) {
    header_text += "Content-Type: application/json\r\n";
  }
  for (const auto& h : headers) {
    header_text += h + "\r\n";
  }
  const std::wstring wide_headers = Widen(header_text);
  if (!WinHttpSendRequest(request.h, wide_headers.c_str(), DWORD(-1),
                          body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                          DWORD(body.size()), DWORD(body.size()), 0)) {
    response.error = WinHttpErrorText(GetLastError());
    return response;
  }
  if (!WinHttpReceiveResponse(request.h, nullptr)) {
    response.error = WinHttpErrorText(GetLastError());
    return response;
  }

  DWORD status = 0;
  DWORD size = sizeof(status);
  if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
    response.error = "no status in the response";
    return response;
  }
  response.status = int(status);

  while (true) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) {
      break;
    }
    if (response.body.size() + available > 8u * 1024 * 1024) {
      response.error = "the response is too large";
      break;
    }
    const size_t offset = response.body.size();
    response.body.resize(offset + available);
    DWORD read = 0;
    if (!WinHttpReadData(request.h, response.body.data() + offset, available, &read)) {
      response.body.resize(offset);
      break;
    }
    response.body.resize(offset + read);
    if (read == 0) {
      break;
    }
  }
  return response;
}

#else

// --- Plain sockets (HTTP only) -------------------------------------------------------

namespace {

void SetNonBlocking(socket_t s, bool on) {
  int flags = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
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
    if (e != EINPROGRESS && e != EWOULDBLOCK) {
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
    int n = int(send(s, data.data() + sent, data.size() - sent, 0));
    if (n <= 0) {
      return false;
    }
    sent += size_t(n);
  }
  return true;
}

}  // namespace

HttpResponse HttpRequest(const std::string& method, const HttpUrl& url, const std::string& path,
                         const std::string& body, const std::vector<std::string>& headers,
                         uint32_t timeout_ms) {
  HttpStartup();
  HttpResponse response;
  if (url.https) {
    response.error = "HTTPS needs the Windows build (WinHTTP); use an http:// address here";
    return response;
  }

  uint32_t ip = ResolveIpv4(url.host);
  if (!ip) {
    response.error = "could not resolve " + url.host;
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
  addr.sin_port = htons(url.port);
  addr.sin_addr.s_addr = htonl(ip);
  if (!ConnectWithTimeout(s, addr, timeout_ms, &response.error)) {
    CloseSocket(s);
    return response;
  }

  std::string request = method + " " + url.base_path + path + " HTTP/1.1\r\n";
  request += "Host: " + url.host + ":" + std::to_string(url.port) + "\r\n";
  request += "User-Agent: mcla-recomp-online/1\r\n";
  request += "Accept: application/json\r\n";
  request += "Connection: close\r\n";
  for (const auto& h : headers) {
    request += h + "\r\n";
  }
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

  std::string raw;
  size_t header_end = std::string::npos;
  size_t content_length = std::string::npos;
  const uint64_t deadline = NowMs() + timeout_ms;
  char buf[8192];
  while (true) {
    uint64_t now = NowMs();
    if (now >= deadline || !WaitFor(s, false, uint32_t(deadline - now))) {
      response.error = "response timed out";
      break;
    }
    int n = int(recv(s, buf, sizeof(buf), 0));
    if (n <= 0) {
      break;
    }
    raw.append(buf, size_t(n));
    if (header_end == std::string::npos) {
      header_end = raw.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        std::string hdrs = Lower(raw.substr(0, header_end));
        size_t cl = hdrs.find("content-length:");
        if (cl != std::string::npos) {
          content_length = size_t(std::atoll(hdrs.c_str() + cl + 15));
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

#endif

}  // namespace mcla::online

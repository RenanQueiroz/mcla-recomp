// The HTTP client the online module talks to the server with: JSON in and out,
// one request per call, every wait bounded. On Windows it is WinHTTP, so HTTPS
// (the usual way a hosted site is reached) and the system proxy come with it;
// elsewhere a plain-socket HTTP/1.1 client, HTTP only.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mcla::online {

// Where the server is: "host:port" (HTTP), or a URL, "https://example.com" or
// "http://host:36000/mcla" (the path is where the app is mounted).
struct HttpUrl {
  bool https = false;
  std::string host;
  uint16_t port = 0;
  std::string base_path;  // "" or "/something", no slash at the end

  // "https://example.com" or "http://host:36000", plus the base path.
  std::string ToString() const;
};

bool ParseHttpUrl(std::string_view text, HttpUrl* out, uint16_t default_port);

struct HttpResponse {
  int status = 0;        // 0: no answer (connection failed or timed out)
  std::string body;
  std::string error;     // why status is 0
  bool ok() const { return status >= 200 && status < 300; }
};

// Starts the host's socket library; safe to call more than once.
void HttpStartup();

// `method` is GET, POST, PUT or DELETE; `path` starts with a slash and goes
// after the URL's base path. `body` is sent as application/json when not
// empty. `headers` are extra lines without the line ending.
HttpResponse HttpRequest(const std::string& method, const HttpUrl& url, const std::string& path,
                         const std::string& body, const std::vector<std::string>& headers,
                         uint32_t timeout_ms = 4000);

// The address of the local interface that routes to `host:port`, in host byte
// order; 0 when unknown.
uint32_t LocalAddressTowards(const std::string& host, uint16_t port);

// Resolves `host` to an IPv4 address in host byte order; 0 on failure.
uint32_t ResolveIpv4(const std::string& host);

}  // namespace mcla::online

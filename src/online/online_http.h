// A minimal blocking HTTP/1.1 client over plain sockets: one request per
// connection, JSON in and out. Enough for the private server's small API and
// free of any dependency the SDK does not already have.

#pragma once

#include <cstdint>
#include <string>

namespace mcla::online {

struct HttpResponse {
  int status = 0;        // 0: no answer (connection failed or timed out)
  std::string body;
  std::string error;     // why status is 0
  bool ok() const { return status >= 200 && status < 300; }
};

// Starts the host's socket library; safe to call more than once.
void HttpStartup();

// `method` is GET, POST, PUT or DELETE. `body` is sent as application/json when
// not empty. Every network wait is bounded by `timeout_ms`.
HttpResponse HttpRequest(const std::string& method, const std::string& host, uint16_t port,
                         const std::string& path, const std::string& body,
                         uint32_t timeout_ms = 4000);

// The address of the local interface that routes to `host:port`, in host byte
// order; 0 when unknown.
uint32_t LocalAddressTowards(const std::string& host, uint16_t port);

// Resolves `host` to an IPv4 address in host byte order; 0 on failure.
uint32_t ResolveIpv4(const std::string& host);

}  // namespace mcla::online

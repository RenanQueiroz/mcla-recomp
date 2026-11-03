// Online play through a private server (fork only).
//
// The game's Xbox LIVE calls (sign-in, XNet secure addressing, QoS, sessions
// and matchmaking through the XGI app) are answered here instead of by the
// SDK's stubs, and the matchmaking state lives on a small HTTP server that one
// player runs (server/mcla-server.js). Game traffic itself stays peer to peer
// over the game's own UDP sockets, which the SDK already maps to real sockets.
//
// The pieces:
//   online_state.cpp   identity, the connection to the server, notifications
//   online_client.cpp  the server's HTTP API
//   online_xnet.cpp    NetDll_XNet*: addresses, keys, QoS, the title server
//   online_user.cpp    XamUser*: signed in to LIVE, privileges, XUIDs
//   online_xgi.cpp     XMsg* and the XGI session messages
//
// The kernel exports are replaced at link time: the recompiled code calls
// __imp__<name>, which rexruntime.dll exports and this module defines too, so
// the linker takes the definitions here and never pulls the DLL's.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "online_json.h"
#include "online_types.h"

namespace mcla::online {

class ServerApi;

struct Identity {
  std::string name;        // the gamertag (the user_name setting)
  uint64_t xuid_online = 0;   // 0x0009...: what LIVE features see
  uint64_t xuid_offline = 0;  // the SDK's profile XUID
  uint64_t machine_id = 0;    // 0xFA00...
  uint8_t mac[6] = {};        // tells consoles apart in an XNADDR
};

// A context or property the game set with XUserSetContext/XUserSetProperty,
// kept to publish with the sessions this player hosts.
struct UserProperty {
  uint32_t id = 0;
  uint8_t type = 0;
  std::vector<uint8_t> payload;  // the value's bytes as the game stores them
};

class OnlineState {
 public:
  static OnlineState& Get();

  // mcla_online is on and the settings parse.
  bool enabled() const { return enabled_; }
  // Registered with the private server (the sign-in state the game sees).
  bool connected() const { return connected_; }
  bool verbose() const { return verbose_; }

  const Identity& identity() const { return identity_; }
  uint32_t title_id() const;

  // Addresses, host byte order.
  uint32_t lan_ip() const { return lan_ip_; }
  uint32_t public_ip() const { return public_ip_; }
  uint16_t game_port() const { return game_port_; }
  // The UDP port the game bound (NetDll_bind): what peers send to.
  void set_game_port(uint16_t port);

  void FillOurXnAddr(XNADDR* out) const;
  bool IsOurXnAddr(const XNADDR& addr) const;
  // The address to send to for a peer's XNADDR: its LAN address when it sits
  // behind the same public address as we do, its public address otherwise.
  uint32_t ResolveXnAddr(const XNADDR& addr) const;
  // An XNADDR from the fields the server keeps for a player or a host.
  static void XnAddrFromJson(const Json& j, XNADDR* out);
  Json OurAddressJson() const;

  ServerApi& server();
  const std::string& server_text() const { return server_text_; }
  // The title server (XNetServerToInAddr) address, 0 when none is configured.
  uint32_t title_server_ip() const { return title_server_ip_; }

  void Start();
  void Stop();

  // The contexts and properties of the local player.
  void SetContext(uint32_t id, uint32_t value);
  void SetProperty(uint32_t id, uint8_t type, const uint8_t* data, size_t size);
  uint32_t GetContext(uint32_t id, uint32_t fallback) const;
  Json ContextsJson() const;
  Json PropertiesJson() const;

 private:
  OnlineState() = default;
  void DeriveIdentity();
  bool Register();
  void HeartbeatThread();
  void BroadcastSignedIn();

  bool enabled_ = false;
  bool verbose_ = false;
  std::atomic<bool> connected_{false};
  bool notified_ = false;
  Identity identity_;
  std::string server_text_;
  uint32_t lan_ip_ = 0;
  uint32_t public_ip_ = 0;
  uint16_t game_port_ = 0;
  uint32_t title_server_ip_ = 0;

  mutable std::mutex mutex_;
  std::vector<UserProperty> contexts_;    // type kDataContext, payload = 4 bytes
  std::vector<UserProperty> properties_;

  std::unique_ptr<ServerApi> server_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
};

// Logging: ONLINE_LOG always, ONLINE_TRACE with mcla_online_log.
#define ONLINE_LOG(...) REXLOG_INFO("online: " __VA_ARGS__)
#define ONLINE_WARN(...) REXLOG_WARN("online: " __VA_ARGS__)
#define ONLINE_TRACE(...)                                      \
  do {                                                         \
    if (::mcla::online::OnlineState::Get().verbose()) {        \
      REXLOG_INFO("online: " __VA_ARGS__);                     \
    }                                                          \
  } while (0)

}  // namespace mcla::online

// Called by the host application (src/mcla_app.cpp).
void MclaOnlineStart();
void MclaOnlineStop();

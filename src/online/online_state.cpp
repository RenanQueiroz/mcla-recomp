// The player's online identity, the connection to the private server, and the
// LIVE sign-in the game is shown.

#include "online.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xnotifylistener.h>

#include "online_client.h"
#include "online_http.h"
#include "online_util.h"

REXCVAR_DEFINE_BOOL(mcla_online, false, "MCLA",
                    "Sign the profile in to a private multiplayer server (mcla_online_server) "
                    "so that the game's online modes work: the server keeps the list of "
                    "sessions, the races themselves run between the players' PCs over UDP. One "
                    "player runs server/mcla-server.js; everyone sets the same address here. "
                    "Takes effect at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_online_server, "127.0.0.1:36000", "MCLA",
                      "The private server's address, host:port (mcla_online). Takes effect at "
                      "the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_online_title_server, "", "MCLA",
                      "An IPv4 address to give the game for Rockstar's title server "
                      "(XNetServerToInAddr); empty, the game is told there is none. Its "
                      "protocol is not implemented: this is for experiments. Takes effect at the "
                      "next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_online_log, false, "MCLA",
                    "Log every call the game makes to the online layer and every request to "
                    "the private server (for reports)");

namespace mcla::online {

namespace {

constexpr uint32_t kHeartbeatMs = 15000;
constexpr uint16_t kDefaultServerPort = 36000;

}  // namespace

OnlineState& OnlineState::Get() {
  static OnlineState state;
  return state;
}

uint32_t OnlineState::title_id() const {
  auto* ks = rex::system::kernel_state();
  return ks ? ks->title_id() : 0x545407F8;
}

ServerApi& OnlineState::server() {
  if (!server_) {
    server_ = std::make_unique<ServerApi>();
  }
  return *server_;
}

void OnlineState::DeriveIdentity() {
  auto* ks = rex::system::kernel_state();
  const auto* profile = ks ? ks->user_profile() : nullptr;
  identity_.name = profile ? profile->name() : std::string("User");
  identity_.xuid_offline = profile ? profile->xuid() : 0xB13EBABEBABEBABEull;

  // Online XUIDs begin 0x0009; the rest comes from the name so that the same
  // name is the same player on every start and every PC. Two players with one
  // name would collide: the server refuses the second.
  const uint64_t h = Fnv1a64(Lower(identity_.name));
  identity_.xuid_online = 0x0009000000000000ull | (h & 0x0000FFFFFFFFFFFFull);
  // Machine ids begin 0xFA; the MAC gets the locally administered, unicast bits.
  const uint64_t m = Fnv1a64(identity_.name + "|machine");
  identity_.machine_id = 0xFA00000000000000ull | (m & 0x00FFFFFFFFFFFFFFull);
  for (int i = 0; i < 6; ++i) {
    identity_.mac[i] = uint8_t(m >> (8 * i));
  }
  identity_.mac[0] = uint8_t((identity_.mac[0] & 0xFE) | 0x02);
}

void OnlineState::set_game_port(uint16_t port) {
  if (port && port != game_port_) {
    game_port_ = port;
    ONLINE_LOG("the game bound UDP port {}: peers must reach this PC on it (forward it on the "
               "router for play across the internet)",
               port);
  }
}

void OnlineState::FillOurXnAddr(XNADDR* out) const {
  std::memset(out, 0, sizeof(*out));
  out->ina = InAddr::from_host_order(lan_ip_ ? lan_ip_ : 0x7F000001u);
  out->ina_online = InAddr::from_host_order(public_ip_ ? public_ip_ : lan_ip_);
  out->port_online = game_port_;
  std::memcpy(out->enet, identity_.mac, 6);
  out->online.ina = out->ina_online;
  out->online.security_parameter_index = uint32_t(identity_.machine_id & 0xFFFFFFFFu) | 1u;
  out->online.xbox_id = identity_.machine_id;
  out->online.platform_type = 2;  // Xbox 360
}

bool OnlineState::IsOurXnAddr(const XNADDR& addr) const {
  return std::memcmp(addr.enet, identity_.mac, 6) == 0;
}

uint32_t OnlineState::ResolveXnAddr(const XNADDR& addr) const {
  if (IsOurXnAddr(addr)) {
    return lan_ip_ ? lan_ip_ : 0x7F000001u;
  }
  const uint32_t peer_public = addr.ina_online.host_order();
  const uint32_t peer_lan = addr.ina.host_order();
  if (peer_public == 0) {
    return peer_lan;
  }
  // Behind the same router as we are: the public address would not route
  // back in on most home routers, the LAN one does.
  if (public_ip_ && peer_public == public_ip_ && peer_lan) {
    return peer_lan;
  }
  return peer_public;
}

void OnlineState::XnAddrFromJson(const Json& j, XNADDR* out) {
  std::memset(out, 0, sizeof(*out));
  out->ina = InAddr::from_host_order(ParseIpv4(j["lanIp"].as_string()));
  out->ina_online = InAddr::from_host_order(ParseIpv4(j["publicIp"].as_string()));
  out->port_online = uint16_t(j["port"].as_u32());
  const std::vector<uint8_t> mac = ParseHexBytes(j["mac"].as_string());
  if (mac.size() == 6) {
    std::memcpy(out->enet, mac.data(), 6);
  }
  const uint64_t machine_id = ParseHex64(j["machineId"].as_string());
  out->online.ina = out->ina_online;
  out->online.security_parameter_index = uint32_t(machine_id & 0xFFFFFFFFu) | 1u;
  out->online.xbox_id = machine_id;
  out->online.platform_type = 2;
}

Json OnlineState::OurAddressJson() const {
  Json j = Json::Object();
  j.set("xuid", Hex64(identity_.xuid_online));
  j.set("name", identity_.name);
  j.set("machineId", Hex64(identity_.machine_id));
  j.set("mac", HexBytes(identity_.mac, 6));
  j.set("lanIp", Ipv4ToString(lan_ip_));
  j.set("port", int(game_port_));
  j.set("titleId", int(title_id()));
  return j;
}

bool OnlineState::Register() {
  Json reply;
  if (!server().RegisterPlayer(OurAddressJson(), &reply)) {
    return false;
  }
  const uint32_t seen = ParseIpv4(reply["publicIp"].as_string());
  if (seen) {
    public_ip_ = seen;
  }
  if (!lan_ip_) {
    lan_ip_ = seen;
  }
  connected_ = true;
  ONLINE_LOG("signed in to {} as {} (XUID {}); this PC is {} on the LAN, {} to the server",
             server_text_, identity_.name, Hex64(identity_.xuid_online), Ipv4ToString(lan_ip_),
             Ipv4ToString(public_ip_));
  return true;
}

void OnlineState::BroadcastSignedIn() {
  auto* ks = rex::system::kernel_state();
  if (!ks) {
    return;
  }
  // XN_SYS_SIGNINCHANGED for user 0, then the LIVE connection and link.
  ks->BroadcastNotification(kNotifySysSigninChanged, 1);
  ks->BroadcastNotification(kNotifyLiveLinkStateChanged, 1);
  ks->BroadcastNotification(kNotifyLiveConnectionChanged, kOnlineLogonConnectionEstablished);
  ONLINE_LOG("told the game the profile is signed in to LIVE");
}

void OnlineState::HeartbeatThread() {
  uint64_t next_heartbeat = NowMs() + kHeartbeatMs;
  uint64_t next_retry = NowMs() + 5000;
  while (!stop_) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const uint64_t now = NowMs();
    auto* ks = rex::system::kernel_state();

    if (connected_ && !notified_ && ks) {
      // The game polls notifications through listeners it creates after it
      // starts; a notification sent before that is lost.
      std::vector<rex::system::object_ref<rex::system::XNotifyListener>> listeners =
          ks->object_table()->GetObjectsByType<rex::system::XNotifyListener>();
      if (!listeners.empty()) {
        BroadcastSignedIn();
        notified_ = true;
      }
    }

    if (!connected_) {
      if (now >= next_retry) {
        next_retry = now + 10000;
        if (Register()) {
          next_heartbeat = now + kHeartbeatMs;
        }
      }
      continue;
    }
    if (now >= next_heartbeat) {
      next_heartbeat = now + kHeartbeatMs;
      if (!server().Heartbeat(identity_.xuid_online, OurAddressJson())) {
        // Re-register on the next round: the server may have restarted.
        ONLINE_WARN("the server did not answer the heartbeat; signing in again");
        connected_ = false;
        next_retry = now + 2000;
      }
    }
  }
}

void OnlineState::Start() {
  enabled_ = REXCVAR_GET(mcla_online);
  verbose_ = REXCVAR_GET(mcla_online_log);
  if (!enabled_) {
    ONLINE_LOG("off (mcla_online = false): the game stays offline");
    return;
  }
  server_text_ = Trim(REXCVAR_GET(mcla_online_server));
  std::string host;
  uint16_t port = 0;
  if (!SplitHostPort(server_text_, &host, &port, kDefaultServerPort)) {
    ONLINE_WARN("mcla_online_server '{}' is not host:port; the game stays offline", server_text_);
    enabled_ = false;
    return;
  }
  HttpStartup();
  server().Configure(host, port);
  DeriveIdentity();
  // The address this PC has on the network the server is on; with the server
  // on this PC that is the loopback, so the address of the default route is
  // taken instead (a UDP connect sends nothing).
  lan_ip_ = LocalAddressTowards(host, port);
  if (!lan_ip_ || (lan_ip_ >> 24) == 127) {
    const uint32_t route = LocalAddressTowards("8.8.8.8", 53);
    if (route && (route >> 24) != 127) {
      lan_ip_ = route;
    }
  }
  const std::string title_server = Trim(REXCVAR_GET(mcla_online_title_server));
  if (!title_server.empty()) {
    title_server_ip_ = ResolveIpv4(title_server);
    if (!title_server_ip_) {
      ONLINE_WARN("mcla_online_title_server '{}' is not an address; ignored", title_server);
    }
  }
  if (!Register()) {
    ONLINE_WARN("could not reach the private server at {}; the game starts offline and signs "
                "in once the server answers",
                server_text_);
  }
  stop_ = false;
  thread_ = std::thread([this] { HeartbeatThread(); });
}

void OnlineState::Stop() {
  stop_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  if (connected_) {
    server().Unregister(identity_.xuid_online);
    connected_ = false;
  }
}

void OnlineState::SetContext(uint32_t id, uint32_t value) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& c : contexts_) {
    if (c.id == id) {
      c.payload.assign({uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8),
                        uint8_t(value)});
      return;
    }
  }
  UserProperty c;
  c.id = id;
  c.type = kDataContext;
  c.payload = {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
  contexts_.push_back(std::move(c));
}

void OnlineState::SetProperty(uint32_t id, uint8_t type, const uint8_t* data, size_t size) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& p : properties_) {
    if (p.id == id) {
      p.type = type;
      p.payload.assign(data, data + size);
      return;
    }
  }
  UserProperty p;
  p.id = id;
  p.type = type;
  p.payload.assign(data, data + size);
  properties_.push_back(std::move(p));
}

uint32_t OnlineState::GetContext(uint32_t id, uint32_t fallback) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& c : contexts_) {
    if (c.id == id && c.payload.size() == 4) {
      return (uint32_t(c.payload[0]) << 24) | (uint32_t(c.payload[1]) << 16) |
             (uint32_t(c.payload[2]) << 8) | c.payload[3];
    }
  }
  return fallback;
}

Json OnlineState::ContextsJson() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Json arr = Json::Array();
  for (const auto& c : contexts_) {
    if (c.payload.size() != 4) {
      continue;
    }
    Json j = Json::Object();
    j.set("id", double(c.id));
    j.set("value", double((uint32_t(c.payload[0]) << 24) | (uint32_t(c.payload[1]) << 16) |
                          (uint32_t(c.payload[2]) << 8) | c.payload[3]));
    arr.push_back(std::move(j));
  }
  return arr;
}

Json OnlineState::PropertiesJson() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Json arr = Json::Array();
  for (const auto& p : properties_) {
    Json j = Json::Object();
    j.set("id", double(p.id));
    j.set("type", int(p.type));
    j.set("data", HexBytes(p.payload.data(), p.payload.size()));
    arr.push_back(std::move(j));
  }
  return arr;
}

}  // namespace mcla::online

void MclaOnlineStart() {
  mcla::online::OnlineState::Get().Start();
}

void MclaOnlineStop() {
  mcla::online::OnlineState::Get().Stop();
}

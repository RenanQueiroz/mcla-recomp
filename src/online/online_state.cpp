// The player's online identity, the connection to the online server, and the
// LIVE sign-in the game is shown.
//
// The player makes an account on the server's website and puts the account's
// game key in mcla.toml (mcla_online_key). The game signs in with the key; the
// server answers with the account's gamertag and online XUID, which become the
// identity other players see. Without a key, or with one the server refuses,
// the game stays offline as it did before.

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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

REXCVAR_DEFINE_BOOL(mcla_online, false, "MCLA",
                    "Sign in to the online server (mcla_online_server) with the account's game key "
                    "(mcla_online_key) so that the game's online modes work: the server keeps the "
                    "list of sessions, the races themselves run between the players' PCs over UDP. "
                    "Accounts are made on the server's website. Takes effect at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_online_server, "127.0.0.1:36000", "MCLA",
                      "The online server: host:port, or its address as a URL (https://example.com, "
                      "http://host:36000/path), as the website's account page shows it. Takes "
                      "effect at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_online_key, "", "MCLA",
                      "The game key of your account on the online server's website: what signs "
                      "the game in. Treat it like a password. Empty: the game stays offline. Takes "
                      "effect at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_online_title_server, "", "MCLA",
                      "An IPv4 address to give the game for Rockstar's title server "
                      "(XNetServerToInAddr); empty, the game is told there is none. Its "
                      "protocol is not implemented: this is for experiments. Takes effect at the "
                      "next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_online_log, false, "MCLA",
                    "Log every call the game makes to the online layer and every request to "
                    "the online server (for reports)");

namespace mcla::online {

namespace {

constexpr uint32_t kHeartbeatMs = 15000;
constexpr uint16_t kDefaultServerPort = 36000;

// Something that names this PC and Windows user and stays the same between
// starts: the machine id and MAC address of the console the game thinks it is.
std::string MachineFingerprint() {
  std::string text;
#ifdef _WIN32
  wchar_t computer[256];
  DWORD n = 256;
  if (GetComputerNameW(computer, &n)) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, computer, int(n), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, computer, int(n), s.data(), bytes, nullptr, nullptr);
    text += s;
  }
  wchar_t user[256];
  DWORD m = 256;
  if (GetUserNameW(user, &m) && m > 1) {
    int bytes = WideCharToMultiByte(CP_UTF8, 0, user, int(m - 1), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, user, int(m - 1), s.data(), bytes, nullptr, nullptr);
    text += "|" + s;
  }
#else
  char host[256] = {};
  if (gethostname(host, sizeof(host) - 1) == 0) {
    text += host;
  }
  if (const char* user = getenv("USER")) {
    text += std::string("|") + user;
  }
#endif
  return text.empty() ? std::string("mcla") : text;
}

bool IsGamertag(const std::string& s) {
  if (s.empty() || s.size() > 15) {
    return false;
  }
  for (unsigned char c : s) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ')) {
      return false;
    }
  }
  return true;
}

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

std::string OnlineState::DisplayName() const {
  if (connected_ && !identity_.name.empty()) {
    return identity_.name;
  }
  auto* ks = rex::system::kernel_state();
  return ks && ks->user_profile() ? ks->user_profile()->name() : std::string("User");
}

void OnlineState::DeriveIdentity() {
  auto* ks = rex::system::kernel_state();
  const auto* profile = ks ? ks->user_profile() : nullptr;
  identity_.name = profile ? profile->name() : std::string("User");
  identity_.xuid_offline = profile ? profile->xuid() : 0xB13EBABEBABEBABEull;
  identity_.xuid_online = 0;  // the account's, once signed in

  // Machine ids begin 0xFA; the MAC gets the locally administered, unicast bits.
  const std::string fingerprint = MachineFingerprint();
  const uint64_t m = Fnv1a64(fingerprint);
  identity_.machine_id = 0xFA00000000000000ull | (m & 0x00FFFFFFFFFFFFFFull);
  const uint64_t mac = Fnv1a64(fingerprint + "|mac");
  for (int i = 0; i < 6; ++i) {
    identity_.mac[i] = uint8_t(mac >> (8 * i));
  }
  identity_.mac[0] = uint8_t((identity_.mac[0] & 0xFE) | 0x02);
}

void OnlineState::set_game_port(uint16_t port) {
  if (port && port != game_port_) {
    game_port_ = port;
    ONLINE_LOG("the game bound UDP port {}: peers must reach this PC on it (forward it on the "
               "router to host races across the internet)",
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
  const int status = server().RegisterPlayer(OurAddressJson(), &reply);
  if (status == 0) {
    return false;  // not reached; the heartbeat thread tries again
  }
  if (status == 401 || status == 403) {
    if (!key_rejected_) {
      ONLINE_WARN("the server at {} refused the game key (mcla_online_key): make a new key on "
                  "your account page at {} and put it in mcla.toml; the game stays offline",
                  server_text_, website_url_);
    }
    key_rejected_ = true;
    return false;
  }
  if (status < 200 || status >= 300) {
    ONLINE_WARN("the server at {} answered HTTP {} to the sign-in; the game stays offline for now",
                server_text_, status);
    return false;
  }
  const uint64_t xuid = ParseHex64(reply["xuid"].as_string());
  const std::string name = Trim(reply["name"].as_string());
  if (!xuid || !IsGamertag(name)) {
    ONLINE_WARN("the server's sign-in answer has no usable account (xuid '{}', name '{}')",
                reply["xuid"].as_string(), name);
    return false;
  }
  identity_.xuid_online = xuid;
  identity_.name = name;
  const uint32_t seen = ParseIpv4(reply["publicIp"].as_string());
  if (seen) {
    public_ip_ = seen;
  }
  if (!lan_ip_) {
    lan_ip_ = seen;
  }
  key_rejected_ = false;
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
  ONLINE_LOG("told the game the profile is signed in to LIVE as {}", identity_.name);
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
        // A refused key is tried again rarely: it changes only when the player
        // edits mcla.toml, which needs a restart anyway.
        next_retry = now + (key_rejected_ ? 60000 : 10000);
        if (Register()) {
          next_heartbeat = now + kHeartbeatMs;
        }
      }
      continue;
    }
    if (now >= next_heartbeat) {
      next_heartbeat = now + kHeartbeatMs;
      Json reply;
      const int status = server().Heartbeat(OurAddressJson(), &reply);
      if (status < 200 || status >= 300) {
        // Sign in again on the next round: the server may have restarted, or
        // the key may have been renewed on the website.
        ONLINE_WARN("the server did not take the heartbeat ({}); signing in again",
                    status ? "HTTP " + std::to_string(status) : "no answer");
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
  HttpUrl url;
  if (!ParseHttpUrl(server_text_, &url, kDefaultServerPort)) {
    ONLINE_WARN("mcla_online_server '{}' is not host:port or a URL; the game stays offline",
                server_text_);
    enabled_ = false;
    return;
  }
  website_url_ = url.ToString();
  HttpStartup();
  const std::string key = Trim(REXCVAR_GET(mcla_online_key));
  server().Configure(url, key);
  DeriveIdentity();

  // The address this PC has on the network the server is on; with the server
  // on this PC that is the loopback, so the address of the default route is
  // taken instead (a UDP connect sends nothing).
  lan_ip_ = LocalAddressTowards(url.host, url.port);
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

  if (key.empty()) {
    key_missing_ = true;
    ONLINE_WARN("mcla_online_key is empty: create an account at {} and put its game key in "
                "mcla.toml; the game stays offline",
                website_url_);
    return;
  }
  if (!Register()) {
    if (!key_rejected_) {
      ONLINE_WARN("could not reach the online server at {}; the game starts offline and signs "
                  "in once the server answers",
                  server_text_);
    }
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

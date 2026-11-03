// XMsg* and the XGI app (0xFB): sessions, matchmaking, arbitration and stats.
//
// The game talks to the xam "XGI" app through XMsgStartIORequest and
// XMsgInProcessCall with a message id and a buffer. The session messages are
// answered here, against the private server; everything else (achievements,
// the music player app, the XLiveBase app) goes on to the SDK's apps.
//
// A session is a kernel object the game names by the guest X_KSESSION the SDK
// hands out from XamSessionRefObjByHandle; both of those exports are replaced
// too, so that every session gets an object of its own.

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xam/app_manager.h>
#include <rex/system/xio.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>

#include "online.h"
#include "online_client.h"
#include "online_util.h"

namespace mcla::online {
void CacheQosData(uint64_t session_id, std::vector<uint8_t> data);  // online_xnet.cpp
}

namespace {

using namespace mcla::online;
namespace sys = rex::system;

constexpr uint32_t kAppXgi = 0xFB;
constexpr uint32_t kAppXLiveBase = 0xFC;

constexpr uint32_t kESuccess = 0;
constexpr uint32_t kEFail = 0x80004005;
constexpr uint32_t kEInvalidArg = 0x80070057;
constexpr uint32_t kEInsufficientBuffer = 0x8007007A;
constexpr uint32_t kENotFound = 0x80070490;
constexpr uint32_t kErrorIoPending = 997;
constexpr uint32_t kErrorNotFound = 0x490;
constexpr uint32_t kErrorInvalidHandle = 6;

constexpr uint32_t kContextGameType = 0x0000800A;
constexpr uint32_t kContextGameMode = 0x0000800B;

constexpr uint32_t kLiveFlags = kSessionPresence | kSessionMatchmaking | kSessionPeerNetwork;

std::mutex g_sessions_mutex;

sys::KernelState* KS() {
  return sys::kernel_state();
}
rex::memory::Memory* Mem() {
  return KS()->memory();
}
template <typename T>
T* Guest(uint32_t addr) {
  return addr ? Mem()->TranslateVirtual<T*>(addr) : nullptr;
}

struct Member {
  uint64_t xuid = 0;
  uint32_t user_index = 0xFF;
  bool is_private = false;
  bool zombie = false;
};

// One session: the kernel object the game holds a handle to, and what we know
// about it. Hosts publish theirs to the server; joiners mirror the host's.
class SessionObject final : public sys::XObject {
 public:
  explicit SessionObject(sys::KernelState* kernel_state)
      : XObject(kernel_state, Type::Session) {
    auto* native = CreateNative<X_KSESSION>();
    native->handle = handle();
  }

  uint64_t id = 0;
  uint32_t flags = 0;
  bool host = false;
  bool published = false;  // on the server
  uint32_t state = kStateLobby;
  uint32_t host_user_index = 0xFF;
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  uint64_t nonce = 0;
  uint32_t game_type = 0;
  uint32_t game_mode = 0;
  XSESSION_INFO info{};
  std::vector<Member> members;

  uint32_t FilledPublic() const {
    uint32_t n = 0;
    for (const auto& m : members) n += !m.is_private;
    return n;
  }
  uint32_t FilledPrivate() const {
    uint32_t n = 0;
    for (const auto& m : members) n += m.is_private;
    return n;
  }
  uint32_t OpenPublic() const { return max_public > FilledPublic() ? max_public - FilledPublic() : 0; }
  uint32_t OpenPrivate() const {
    return max_private > FilledPrivate() ? max_private - FilledPrivate() : 0;
  }
  Member* Find(uint64_t xuid) {
    for (auto& m : members) {
      if (m.xuid == xuid) return &m;
    }
    return nullptr;
  }
};

SessionObject* LookupSession(uint32_t obj_ptr) {
  const auto* ksession = Guest<X_KSESSION>(obj_ptr);
  if (!ksession) {
    return nullptr;
  }
  const uint32_t handle = ksession->handle;
  auto object = KS()->object_table()->LookupObject<sys::XObject>(handle);
  if (!object || object->type() != sys::XObject::Type::Session) {
    return nullptr;
  }
  // The handle keeps the object alive; the reference the lookup took can go.
  return static_cast<SessionObject*>(object.get());
}

Json MembersJson(const SessionObject& s) {
  Json arr = Json::Array();
  for (const auto& m : s.members) {
    Json j = Json::Object();
    j.set("xuid", Hex64(m.xuid));
    j.set("private", m.is_private);
    j.set("userIndex", int(m.user_index));
    arr.push_back(std::move(j));
  }
  return arr;
}

Json SessionJson(const SessionObject& s) {
  auto& state = OnlineState::Get();
  Json j = Json::Object();
  j.set("id", Hex64(s.id));
  j.set("titleId", double(state.title_id()));
  j.set("hostXuid", Hex64(state.identity().xuid_online));
  j.set("hostName", state.identity().name);
  j.set("flags", double(s.flags));
  j.set("publicSlots", double(s.max_public));
  j.set("privateSlots", double(s.max_private));
  j.set("state", double(s.state));
  j.set("nonce", Hex64(s.nonce));
  j.set("key", HexBytes(s.info.key_exchange_key.ab, 16));
  j.set("host", state.OurAddressJson());
  j.set("contexts", state.ContextsJson());
  j.set("properties", state.PropertiesJson());
  j.set("members", MembersJson(s));
  return j;
}

void SyncSession(SessionObject& s, const char* what) {
  if (!s.host || !s.published) {
    return;
  }
  auto& state = OnlineState::Get();
  if (!state.connected()) {
    return;
  }
  Json patch = Json::Object();
  patch.set("flags", double(s.flags));
  patch.set("publicSlots", double(s.max_public));
  patch.set("privateSlots", double(s.max_private));
  patch.set("state", double(s.state));
  patch.set("members", MembersJson(s));
  patch.set("contexts", state.ContextsJson());
  patch.set("properties", state.PropertiesJson());
  if (!state.server().UpdateSession(s.id, patch)) {
    ONLINE_WARN("session {}: the server did not take the {} update", Hex64(s.id), what);
  }
}

// Memory inside a buffer the game provided, then the system heap when it is
// full. What the system heap gives out is never freed: the game owns the
// buffers and frees nothing of what the search put behind them.
struct GuestArena {
  uint32_t guest_base = 0;
  uint32_t size = 0;
  uint32_t used = 0;

  uint32_t Alloc(uint32_t bytes, uint32_t align = 8) {
    used = (used + align - 1) & ~(align - 1);
    if (bytes == 0) {
      return 0;
    }
    if (guest_base && used + bytes <= size) {
      const uint32_t addr = guest_base + used;
      used += bytes;
      std::memset(Guest<uint8_t>(addr), 0, bytes);
      return addr;
    }
    const uint32_t addr = Mem()->SystemHeapAlloc(bytes);
    if (addr) {
      std::memset(Guest<uint8_t>(addr), 0, bytes);
    }
    return addr;
  }
};

// A property from the server (id, type, data as hex) into the game's form.
void WriteProperty(const Json& p, XUSER_PROPERTY* out, GuestArena& arena) {
  std::memset(out, 0, sizeof(*out));
  const uint32_t id = p["id"].as_u32();
  const uint8_t type = uint8_t(p["type"].as_u32(PropertyType(id)));
  const std::vector<uint8_t> payload = ParseHexBytes(p["data"].as_string());
  out->property_id = id;
  out->data.type = type;
  switch (type) {
    case kDataInt32:
    case kDataFloat:
      std::memcpy(out->data.raw, payload.data(), std::min<size_t>(4, payload.size()));
      break;
    case kDataInt64:
    case kDataDouble:
    case kDataDateTime:
      std::memcpy(out->data.raw, payload.data(), std::min<size_t>(8, payload.size()));
      break;
    case kDataWString:
    case kDataBinary: {
      const uint32_t addr = arena.Alloc(uint32_t(payload.size()));
      if (addr) {
        std::memcpy(Guest<uint8_t>(addr), payload.data(), payload.size());
      }
      out->data.blob.size = addr ? uint32_t(payload.size()) : 0;
      out->data.blob.ptr = addr;
      break;
    }
    default:
      std::memcpy(out->data.raw, payload.data(), std::min<size_t>(8, payload.size()));
      break;
  }
}

// A property the game passes (XUSER_PROPERTY in guest memory) as JSON.
Json PropertyJson(const XUSER_PROPERTY& p) {
  Json j = Json::Object();
  const uint32_t id = p.property_id;
  const uint8_t type = p.data.type ? p.data.type : PropertyType(id);
  j.set("id", double(id));
  j.set("type", int(type));
  if (type == kDataWString || type == kDataBinary) {
    const uint32_t size = p.data.blob.size;
    const uint32_t ptr = p.data.blob.ptr;
    const uint8_t* bytes = Guest<uint8_t>(ptr);
    j.set("data", (bytes && size && size < 0x10000) ? HexBytes(bytes, size) : std::string());
  } else {
    const size_t n = (type == kDataInt32 || type == kDataFloat) ? 4 : 8;
    j.set("data", HexBytes(p.data.raw, n));
  }
  return j;
}

// --- The messages ----------------------------------------------------------------

uint32_t SessionCreate(const XGI_SESSION_CREATE& d) {
  auto& state = OnlineState::Get();
  SessionObject* s = LookupSession(d.obj_ptr);
  auto* info = Guest<XSESSION_INFO>(d.session_info_ptr);
  auto* nonce = Guest<rex::be<uint64_t>>(d.nonce_ptr);
  if (!s || !info) {
    ONLINE_WARN("XSessionCreate: no session object at {:08X}", uint32_t(d.obj_ptr));
    return kEInvalidArg;
  }
  s->flags = d.flags;
  s->max_public = d.num_slots_public;
  s->max_private = d.num_slots_private;
  s->state = kStateLobby;
  s->game_type = state.GetContext(kContextGameType, 0);
  s->game_mode = state.GetContext(kContextGameMode, 0);

  const bool live = (d.flags & kLiveFlags) != 0;
  const bool creates = (d.flags & kSessionHost) || !live || XnkidToU64(info->session_id) == 0;
  if (creates) {
    s->host = true;
    s->host_user_index = d.user_index;
    s->id = (0x80ull << 56) | (RandomU64() & 0x00FFFFFFFFFFFFFFull) | 1ull;
    s->nonce = RandomU64();
    std::memset(info, 0, sizeof(*info));
    info->session_id = U64ToXnkid(s->id);
    state.FillOurXnAddr(&info->host_address);
    RandomBytes(info->key_exchange_key.ab, 16);
    if (nonce) {
      *nonce = s->nonce;
    }
    s->info = *info;
    if (live && (d.flags & kSessionHost) && state.connected()) {
      s->published = state.server().CreateSession(SessionJson(*s));
      if (!s->published) {
        ONLINE_WARN("session {}: the server did not take it; others cannot find it",
                    Hex64(s->id));
      }
    }
    ONLINE_LOG("XSessionCreate: hosting {} (flags {:#x}, {} public + {} private slots{})",
               Hex64(s->id), uint32_t(d.flags), uint32_t(d.num_slots_public),
               uint32_t(d.num_slots_private), s->published ? ", on the server" : "");
  } else {
    s->host = false;
    s->id = XnkidToU64(info->session_id);
    s->info = *info;
    if (nonce) {
      s->nonce = *nonce;
    }
    // The host's current address, in case the search result is stale.
    Json remote;
    if (state.connected() && state.server().GetSession(s->id, &remote)) {
      XNADDR addr;
      OnlineState::XnAddrFromJson(remote["host"], &addr);
      if (addr.ina.raw || addr.ina_online.raw) {
        info->host_address = addr;
        s->info.host_address = addr;
      }
      s->max_public = remote["publicSlots"].as_u32(s->max_public);
      s->max_private = remote["privateSlots"].as_u32(s->max_private);
      s->nonce = ParseHex64(remote["nonce"].as_string());
      if (nonce && s->nonce) {
        *nonce = s->nonce;
      }
    }
    ONLINE_LOG("XSessionCreate: joining {} hosted at {} / {} (flags {:#x})", Hex64(s->id),
               Ipv4ToString(info->host_address.ina.host_order()),
               Ipv4ToString(info->host_address.ina_online.host_order()), uint32_t(d.flags));
  }
  return kESuccess;
}

uint32_t SessionDelete(const XGI_SESSION_STATE& d) {
  SessionObject* s = LookupSession(d.obj_ptr);
  if (!s) {
    return kEInvalidArg;
  }
  if (s->state == kStateDeleted) {
    return kOnlineESessionWrongState;
  }
  ONLINE_LOG("XSessionDelete: {}", Hex64(s->id));
  if (s->host && s->published && OnlineState::Get().connected()) {
    OnlineState::Get().server().DeleteSession(s->id);
  }
  s->published = false;
  s->state = kStateDeleted;
  // The reference XamSessionCreateHandle took; the game closed its own.
  s->ReleaseHandle();
  return kESuccess;
}

uint32_t SessionJoinLeave(const XGI_SESSION_MANAGE& d, bool join) {
  SessionObject* s = LookupSession(d.obj_ptr);
  if (!s) {
    return kEInvalidArg;
  }
  if (s->state == kStateDeleted) {
    return kOnlineESessionWrongState;
  }
  const bool local = d.xuid_array_ptr == 0;
  if (!d.array_count || (local && !d.indices_array_ptr)) {
    return kEInvalidArg;
  }
  const auto* xuids = Guest<rex::be<uint64_t>>(d.xuid_array_ptr);
  const auto* indices = Guest<rex::be<uint32_t>>(d.indices_array_ptr);
  const auto* privates = Guest<rex::be<uint32_t>>(d.private_slots_array_ptr);
  auto& state = OnlineState::Get();
  const uint32_t count = std::min<uint32_t>(d.array_count, 64);
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t xuid;
    uint32_t user_index = 0xFF;
    if (local) {
      user_index = indices[i];
      xuid = state.identity().xuid_online;
    } else {
      xuid = xuids[i];
      if (xuid == state.identity().xuid_online || xuid == state.identity().xuid_offline) {
        user_index = 0;
      }
    }
    const bool is_private = privates && privates[i] != 0;
    if (join) {
      if (!s->Find(xuid)) {
        if (is_private && s->OpenPrivate() == 0 && s->OpenPublic() == 0) {
          return kOnlineESessionFull;
        }
        if (!is_private && s->OpenPublic() == 0 && s->OpenPrivate() == 0) {
          return kOnlineESessionFull;
        }
        Member m;
        m.xuid = xuid;
        m.user_index = user_index;
        m.is_private = is_private ? true : (s->OpenPublic() == 0);
        s->members.push_back(m);
      }
    } else {
      auto it = std::find_if(s->members.begin(), s->members.end(),
                             [xuid](const Member& m) { return m.xuid == xuid; });
      if (it != s->members.end()) {
        if ((s->flags & kSessionArbitration) && s->state == kStateInGame) {
          it->zombie = true;  // kept for the stats of an arbitrated game
        } else {
          s->members.erase(it);
        }
      }
    }
    ONLINE_TRACE("{}: {} {} {}", join ? "XSessionJoin" : "XSessionLeave", Hex64(s->id),
                 local ? "local user" : "remote", Hex64(xuid));
  }
  SyncSession(*s, join ? "join" : "leave");
  return kESuccess;
}

uint32_t SessionStart(const XGI_SESSION_STATE& d) {
  SessionObject* s = LookupSession(d.obj_ptr);
  if (!s) {
    return kEInvalidArg;
  }
  if (s->state != kStateLobby && s->state != kStateRegistration) {
    return kOnlineESessionWrongState;
  }
  s->state = kStateInGame;
  ONLINE_LOG("XSessionStart: {}", Hex64(s->id));
  SyncSession(*s, "start");
  return kESuccess;
}

uint32_t SessionEnd(const XGI_SESSION_STATE& d) {
  SessionObject* s = LookupSession(d.obj_ptr);
  if (!s) {
    return kEInvalidArg;
  }
  if (s->state != kStateInGame) {
    return kOnlineESessionWrongState;
  }
  // Reporting is over at once; back to the lobby, which can start again.
  s->state = kStateLobby;
  s->members.erase(std::remove_if(s->members.begin(), s->members.end(),
                                  [](const Member& m) { return m.zombie; }),
                   s->members.end());
  ONLINE_LOG("XSessionEnd: {}", Hex64(s->id));
  SyncSession(*s, "end");
  return kESuccess;
}

uint32_t SessionModify(const XGI_SESSION_MODIFY& d) {
  SessionObject* s = LookupSession(d.obj_ptr);
  if (!s) {
    return kEInvalidArg;
  }
  constexpr uint32_t kModifiable = kSessionArbitration | kSessionInvitesDisabled |
                                   kSessionJoinViaPresenceDisabled |
                                   kSessionJoinInProgressDisabled |
                                   kSessionJoinViaPresenceFriendsOnly;
  s->flags = (s->flags & ~kModifiable) | (uint32_t(d.flags) & kModifiable);
  s->max_public = d.max_public_slots;
  s->max_private = d.max_private_slots;
  ONLINE_TRACE("XSessionModify: {} flags {:#x}, {} + {} slots", Hex64(s->id), s->flags,
               s->max_public, s->max_private);
  SyncSession(*s, "modify");
  return kESuccess;
}

uint32_t SessionGetDetails(const XGI_SESSION_DETAILS& d) {
  SessionObject* s = LookupSession(d.obj_ptr);
  auto* size_ptr = Guest<rex::be<uint32_t>>(d.details_buffer_size);
  if (!s || !size_ptr || !d.session_details_ptr) {
    return kEInvalidArg;
  }
  const uint32_t buffer_size = *size_ptr;
  const uint32_t member_count = uint32_t(s->members.size());
  const uint32_t needed = uint32_t(sizeof(XSESSION_LOCAL_DETAILS)) + member_count * 0x10;
  if (buffer_size < sizeof(XSESSION_LOCAL_DETAILS)) {
    *size_ptr = needed;
    return kEInsufficientBuffer;
  }
  auto* details = Guest<XSESSION_LOCAL_DETAILS>(d.session_details_ptr);
  std::memset(details, 0, sizeof(*details));
  details->user_index_host = s->host ? s->host_user_index : 0xFF;
  details->game_type = s->game_type;
  details->game_mode = s->game_mode;
  details->flags = s->flags;
  details->max_public_slots = s->max_public;
  details->max_private_slots = s->max_private;
  details->available_public_slots = s->OpenPublic();
  details->available_private_slots = s->OpenPrivate();
  details->state = s->state;
  details->nonce = s->nonce;
  details->session_info = s->info;
  const uint32_t room = (buffer_size - uint32_t(sizeof(XSESSION_LOCAL_DETAILS))) / 0x10;
  const uint32_t returned = std::min(room, member_count);
  auto* members = reinterpret_cast<XSESSION_MEMBER*>(details + 1);
  for (uint32_t i = 0; i < returned; ++i) {
    members[i].online_xuid = s->members[i].xuid;
    members[i].user_index = s->members[i].user_index;
    members[i].flags = (s->members[i].is_private ? 1u : 0u) | (s->members[i].zombie ? 2u : 0u);
  }
  details->actual_member_count = member_count;
  details->returned_member_count = returned;
  details->session_members_ptr = returned ? d.session_details_ptr + sizeof(XSESSION_LOCAL_DETAILS) : 0;
  *size_ptr = uint32_t(sizeof(XSESSION_LOCAL_DETAILS)) + returned * 0x10;
  ONLINE_TRACE("XSessionGetDetails: {} state {} members {}", Hex64(s->id), s->state,
               member_count);
  return kESuccess;
}

uint32_t SessionMigrate(const XGI_SESSION_MIGRATE& d) {
  SessionObject* s = LookupSession(d.obj_ptr);
  auto* info = Guest<XSESSION_INFO>(d.session_info_ptr);
  if (!s || !info) {
    return kEInvalidArg;
  }
  auto& state = OnlineState::Get();
  const uint64_t old_id = s->id;
  if (d.user_index < 4) {
    // We are the new host.
    s->host = true;
    s->host_user_index = d.user_index;
    s->id = (0x80ull << 56) | (RandomU64() & 0x00FFFFFFFFFFFFFFull) | 1ull;
    s->info.session_id = U64ToXnkid(s->id);
    state.FillOurXnAddr(&s->info.host_address);
    *info = s->info;
    if ((s->flags & kLiveFlags) && state.connected()) {
      s->published = state.server().MigrateSession(old_id, SessionJson(*s));
    }
    ONLINE_LOG("XSessionMigrateHost: {} is now {} with this PC as host", Hex64(old_id),
               Hex64(s->id));
  } else {
    s->host = false;
    s->published = false;
    s->info = *info;
    s->id = XnkidToU64(info->session_id);
    ONLINE_LOG("XSessionMigrateHost: {} moved to {} at {} / {}", Hex64(old_id), Hex64(s->id),
               Ipv4ToString(info->host_address.ina.host_order()),
               Ipv4ToString(info->host_address.ina_online.host_order()));
  }
  return kESuccess;
}

uint32_t SessionArbitrationRegister(const XGI_SESSION_ARBITRATION& d) {
  SessionObject* s = LookupSession(d.obj_ptr);
  auto* size_ptr = Guest<rex::be<uint32_t>>(d.results_buffer_size);
  if (!s || !size_ptr) {
    return kEInvalidArg;
  }
  // One registrant (machine) per member; the server knows machines by XUID.
  auto& state = OnlineState::Get();
  struct Machine {
    uint64_t id;
    std::vector<uint64_t> xuids;
  };
  std::vector<Machine> machines;
  for (const auto& m : s->members) {
    uint64_t machine_id = 0;
    if (m.xuid == state.identity().xuid_online) {
      machine_id = state.identity().machine_id;
    } else {
      Json player;
      if (state.connected() && state.server().GetPlayer(m.xuid, &player)) {
        machine_id = ParseHex64(player["machineId"].as_string());
      }
      if (!machine_id) {
        machine_id = 0xFA00000000000000ull | (m.xuid & 0x00FFFFFFFFFFFFFFull);
      }
    }
    auto it = std::find_if(machines.begin(), machines.end(),
                           [machine_id](const Machine& x) { return x.id == machine_id; });
    if (it == machines.end()) {
      machines.push_back({machine_id, {m.xuid}});
    } else {
      it->xuids.push_back(m.xuid);
    }
  }
  const uint32_t needed = uint32_t(sizeof(XSESSION_REGISTRATION_RESULTS) +
                                   machines.size() * sizeof(XSESSION_REGISTRANT) +
                                   s->members.size() * sizeof(uint64_t));
  if (*size_ptr < needed || !d.results_ptr) {
    *size_ptr = needed;
    return kEInsufficientBuffer;
  }
  auto* results = Guest<XSESSION_REGISTRATION_RESULTS>(d.results_ptr);
  std::memset(results, 0, needed);
  const uint32_t registrants_addr = d.results_ptr + uint32_t(sizeof(XSESSION_REGISTRATION_RESULTS));
  auto* registrants = Guest<XSESSION_REGISTRANT>(registrants_addr);
  uint32_t xuids_addr =
      registrants_addr + uint32_t(machines.size() * sizeof(XSESSION_REGISTRANT));
  for (size_t i = 0; i < machines.size(); ++i) {
    registrants[i].machine_id = machines[i].id;
    registrants[i].trustworthiness = 1;
    registrants[i].num_users = uint32_t(machines[i].xuids.size());
    registrants[i].users_ptr = xuids_addr;
    auto* xuids = Guest<rex::be<uint64_t>>(xuids_addr);
    for (size_t k = 0; k < machines[i].xuids.size(); ++k) {
      xuids[k] = machines[i].xuids[k];
    }
    xuids_addr += uint32_t(machines[i].xuids.size() * sizeof(uint64_t));
  }
  results->registrants_count = uint32_t(machines.size());
  results->registrants_ptr = registrants_addr;
  *size_ptr = needed;
  s->state = kStateRegistration;
  ONLINE_LOG("XSessionArbitrationRegister: {} with {} machines, {} players", Hex64(s->id),
             machines.size(), s->members.size());
  return kESuccess;
}

uint32_t SessionSearch(const XGI_SESSION_SEARCH& d, uint32_t num_users) {
  auto& state = OnlineState::Get();
  if (!state.connected()) {
    return kOnlineESessionNotLoggedOn;
  }
  if (!d.search_results_ptr || d.results_buffer_size < sizeof(XSESSION_SEARCHRESULT_HEADER)) {
    return kEInvalidArg;
  }
  Json query = Json::Object();
  query.set("titleId", double(state.title_id()));
  query.set("numResults", double(d.num_results));
  query.set("procIndex", double(d.proc_index));
  query.set("excludeXuid", Hex64(state.identity().xuid_online));
  Json contexts = Json::Array();
  if (const auto* ctx = Guest<XUSER_CONTEXT>(d.ctx_ptr)) {
    for (uint32_t i = 0; i < uint32_t(d.num_ctx); ++i) {
      Json c = Json::Object();
      c.set("id", double(ctx[i].context_id));
      c.set("value", double(ctx[i].value));
      contexts.push_back(std::move(c));
    }
  }
  query.set("contexts", std::move(contexts));
  Json properties = Json::Array();
  if (const auto* props = Guest<XUSER_PROPERTY>(d.props_ptr)) {
    for (uint32_t i = 0; i < uint32_t(d.num_props); ++i) {
      properties.push_back(PropertyJson(props[i]));
    }
  }
  query.set("properties", std::move(properties));

  Json reply;
  if (!state.server().SearchSessions(query, &reply)) {
    return kEFail;
  }
  const Json& found = reply.is_array() ? reply : reply["sessions"];

  // The header, then the results, then what each result points to, all in the
  // game's buffer as far as it goes.
  GuestArena arena{d.search_results_ptr, d.results_buffer_size, 0};
  auto* header = Guest<XSESSION_SEARCHRESULT_HEADER>(d.search_results_ptr);
  std::memset(header, 0, sizeof(*header));
  arena.used = sizeof(XSESSION_SEARCHRESULT_HEADER);
  const uint32_t wanted = std::min<uint32_t>(d.num_results, uint32_t(found.size()));
  const uint32_t room =
      (d.results_buffer_size - arena.used) / uint32_t(sizeof(XSESSION_SEARCHRESULT));
  const uint32_t count = std::min(wanted, room);
  const uint32_t results_addr = count ? arena.Alloc(count * uint32_t(sizeof(XSESSION_SEARCHRESULT))) : 0;
  auto* results = Guest<XSESSION_SEARCHRESULT>(results_addr);
  for (uint32_t i = 0; i < count; ++i) {
    const Json& sj = found[i];
    XSESSION_SEARCHRESULT& r = results[i];
    std::memset(&r, 0, sizeof(r));
    const uint64_t id = ParseHex64(sj["id"].as_string());
    r.info.session_id = U64ToXnkid(id);
    OnlineState::XnAddrFromJson(sj["host"], &r.info.host_address);
    const std::vector<uint8_t> key = ParseHexBytes(sj["key"].as_string());
    if (key.size() == 16) {
      std::memcpy(r.info.key_exchange_key.ab, key.data(), 16);
    }
    r.open_public_slots = sj["openPublic"].as_u32();
    r.open_private_slots = sj["openPrivate"].as_u32();
    r.filled_public_slots = sj["filledPublic"].as_u32();
    r.filled_private_slots = sj["filledPrivate"].as_u32();

    const Json& ctxs = sj["contexts"];
    r.contexts_count = uint32_t(ctxs.size());
    if (ctxs.size()) {
      const uint32_t addr = arena.Alloc(uint32_t(ctxs.size() * sizeof(XUSER_CONTEXT)));
      auto* out = Guest<XUSER_CONTEXT>(addr);
      for (size_t k = 0; out && k < ctxs.size(); ++k) {
        out[k].context_id = ctxs[k]["id"].as_u32();
        out[k].value = ctxs[k]["value"].as_u32();
      }
      r.contexts_ptr = addr;
      if (!addr) r.contexts_count = 0;
    }
    const Json& props = sj["properties"];
    r.properties_count = uint32_t(props.size());
    if (props.size()) {
      const uint32_t addr = arena.Alloc(uint32_t(props.size() * sizeof(XUSER_PROPERTY)));
      auto* out = Guest<XUSER_PROPERTY>(addr);
      for (size_t k = 0; out && k < props.size(); ++k) {
        WriteProperty(props[k], &out[k], arena);
      }
      r.properties_ptr = addr;
      if (!addr) r.properties_count = 0;
    }
    if (sj.has("qos") && sj["qos"].is_string() && !sj["qos"].as_string().empty()) {
      CacheQosData(id, Base64Decode(sj["qos"].as_string()));
    }
    ONLINE_TRACE("  found {} at {} / {} port {}, {} + {} open slots", Hex64(id),
                 Ipv4ToString(r.info.host_address.ina.host_order()),
                 Ipv4ToString(r.info.host_address.ina_online.host_order()),
                 uint16_t(r.info.host_address.port_online), uint32_t(r.open_public_slots),
                 uint32_t(r.open_private_slots));
  }
  header->search_results_count = count;
  header->search_results_ptr = results_addr;
  ONLINE_LOG("XSessionSearch(proc {}, {} contexts, {} properties): {} of {} sessions returned",
             uint32_t(d.proc_index), uint32_t(d.num_ctx), uint32_t(d.num_props), count,
             found.size());
  return kESuccess;
}

uint32_t UserReadStats(const XGI_XUSER_READ_STATS& d) {
  // No leaderboards on the private server: every view comes back with no rows,
  // which the game shows as empty rather than failing the screen.
  if (!d.results_ptr || !d.specs_ptr || !d.specs_count) {
    return kEInvalidArg;
  }
  auto* results = Guest<X_USER_STATS_READ_RESULTS>(d.results_ptr);
  const uint32_t views_fit =
      d.results_size > sizeof(X_USER_STATS_READ_RESULTS)
          ? (uint32_t(d.results_size) - uint32_t(sizeof(X_USER_STATS_READ_RESULTS))) /
                uint32_t(sizeof(X_USER_STATS_VIEW))
          : 0;
  const uint32_t num_views = std::min<uint32_t>(d.specs_count, views_fit);
  std::memset(results, 0, sizeof(*results) + num_views * sizeof(X_USER_STATS_VIEW));
  const auto* specs = Guest<X_USER_STATS_SPEC>(d.specs_ptr);
  auto* views = reinterpret_cast<X_USER_STATS_VIEW*>(results + 1);
  for (uint32_t i = 0; i < num_views; ++i) {
    views[i].view_id = specs[i].view_id;
    views[i].total_view_rows = 0;
    views[i].num_rows = 0;
    views[i].rows_ptr = 0;
  }
  results->num_views = num_views;
  results->views_ptr = num_views ? d.results_ptr + uint32_t(sizeof(X_USER_STATS_READ_RESULTS)) : 0;
  ONLINE_TRACE("XUserReadStats: {} users, {} views -> empty", uint32_t(d.xuids_count),
               num_views);
  return kESuccess;
}

uint32_t SessionWriteStats(const XGI_STATS_WRITE& d, bool flush) {
  SessionObject* s = LookupSession(d.obj_ptr);
  if (!s) {
    return kEInvalidArg;
  }
  auto& state = OnlineState::Get();
  if (!flush && state.connected() && d.num_views && d.views_ptr) {
    Json stats = Json::Object();
    stats.set("sessionId", Hex64(s->id));
    stats.set("xuid", Hex64(d.xuid));
    stats.set("titleId", double(state.title_id()));
    Json views = Json::Array();
    const auto* view = Guest<XSESSION_VIEW_PROPERTIES>(d.views_ptr);
    const uint32_t num_views = std::min<uint32_t>(d.num_views, 64);
    for (uint32_t i = 0; i < num_views; ++i) {
      Json v = Json::Object();
      v.set("viewId", double(view[i].view_id));
      Json props = Json::Array();
      const auto* p = Guest<XUSER_PROPERTY>(view[i].properties_ptr);
      const uint32_t n = std::min<uint32_t>(view[i].properties_count, 64);
      for (uint32_t k = 0; p && k < n; ++k) {
        props.push_back(PropertyJson(p[k]));
      }
      v.set("properties", std::move(props));
      views.push_back(std::move(v));
    }
    stats.set("views", std::move(views));
    state.server().WriteStats(stats);
  }
  ONLINE_TRACE("{}: {} for {} ({} views)", flush ? "XSessionFlushStats" : "XSessionWriteStats",
               Hex64(s->id), Hex64(d.xuid), uint32_t(d.num_views));
  return kESuccess;
}

uint32_t UserSetContext(const XGI_XUSER_SET_CONTEXT& d) {
  auto& state = OnlineState::Get();
  state.SetContext(d.context.context_id, d.context.value);
  ONLINE_TRACE("XUserSetContext({:#x} = {})", uint32_t(d.context.context_id),
               uint32_t(d.context.value));
  return kESuccess;
}

uint32_t UserSetProperty(const XGI_XUSER_SET_PROPERTY& d) {
  const auto* data = Guest<uint8_t>(d.data_address);
  const uint32_t size = d.data_size;
  if (data && size && size <= 0x1000) {
    OnlineState::Get().SetProperty(d.property_id, PropertyType(d.property_id), data, size);
  }
  ONLINE_TRACE("XUserSetProperty({:#x}, {} bytes)", uint32_t(d.property_id), size);
  return kESuccess;
}

// XLiveBase (0xFC) messages answered here: the few the game needs a value from
// that the SDK fails.
bool HandleXLiveBase(uint32_t message, uint32_t buffer_ptr, uint32_t* result) {
  switch (message) {
    case 0x00058009: {  // XContentGetMarketplaceCounts: nothing to buy
      struct Request {
        rex::be<uint32_t> user_index;
        rex::be<uint32_t> title_id;
        rex::be<uint32_t> content_categories;
        rex::be<uint32_t> results_ptr;
      };
      const auto* request = Guest<Request>(buffer_ptr);
      if (request) {
        if (auto* out = Guest<uint32_t>(request->results_ptr)) {
          out[0] = 0;  // new offers
          out[1] = 0;  // total offers
        }
      }
      *result = kESuccess;
      return true;
    }
    case 0x0005800E: {  // XUserMuteListQuery: nobody is muted
      struct Request {
        rex::be<uint32_t> user_index;
        rex::be<uint64_t> remote_xuid;
        rex::be<uint32_t> result;
      };
      if (auto* request = Guest<Request>(buffer_ptr)) {
        request->result = 0;
      }
      *result = kESuccess;
      return true;
    }
    default:
      return false;
  }
}

template <typename T>
const T* MessageData(uint32_t buffer_ptr, uint32_t buffer_length) {
  if (!buffer_ptr || (buffer_length && buffer_length < sizeof(T))) {
    return nullptr;
  }
  return Guest<T>(buffer_ptr);
}

bool HandleXgi(uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length, uint32_t* result) {
  std::lock_guard<std::mutex> lock(g_sessions_mutex);
  switch (message) {
    case 0x000B0006:
      if (auto* d = MessageData<XGI_XUSER_SET_CONTEXT>(buffer_ptr, buffer_length)) {
        *result = UserSetContext(*d);
        return true;
      }
      return false;
    case 0x000B0007:
      if (auto* d = MessageData<XGI_XUSER_SET_PROPERTY>(buffer_ptr, buffer_length)) {
        *result = UserSetProperty(*d);
        return true;
      }
      return false;
    case 0x000B0010: {
      auto* d = MessageData<XGI_SESSION_CREATE>(buffer_ptr, buffer_length);
      *result = d ? SessionCreate(*d) : kEInvalidArg;
      return true;
    }
    case 0x000B0011: {
      auto* d = MessageData<XGI_SESSION_STATE>(buffer_ptr, buffer_length);
      *result = d ? SessionDelete(*d) : kEInvalidArg;
      return true;
    }
    case 0x000B0012:
    case 0x000B0013: {
      auto* d = MessageData<XGI_SESSION_MANAGE>(buffer_ptr, buffer_length);
      *result = d ? SessionJoinLeave(*d, message == 0x000B0012) : kEInvalidArg;
      return true;
    }
    case 0x000B0014: {
      auto* d = MessageData<XGI_SESSION_STATE>(buffer_ptr, buffer_length);
      *result = d ? SessionStart(*d) : kEInvalidArg;
      return true;
    }
    case 0x000B0015: {
      auto* d = MessageData<XGI_SESSION_STATE>(buffer_ptr, buffer_length);
      *result = d ? SessionEnd(*d) : kEInvalidArg;
      return true;
    }
    case 0x000B0018: {
      auto* d = MessageData<XGI_SESSION_MODIFY>(buffer_ptr, buffer_length);
      *result = d ? SessionModify(*d) : kEInvalidArg;
      return true;
    }
    case 0x000B001A: {
      auto* d = MessageData<XGI_SESSION_ARBITRATION>(buffer_ptr, buffer_length);
      *result = d ? SessionArbitrationRegister(*d) : kEInvalidArg;
      return true;
    }
    case 0x000B0016: {
      auto* d = MessageData<XGI_SESSION_SEARCH>(buffer_ptr, buffer_length);
      *result = d ? SessionSearch(*d, 1) : kEInvalidArg;
      return true;
    }
    case 0x000B001C: {
      auto* d = MessageData<XGI_SESSION_SEARCH_EX>(buffer_ptr, buffer_length);
      *result = d ? SessionSearch(d->search, d->num_users) : kEInvalidArg;
      return true;
    }
    case 0x000B001D: {
      auto* d = MessageData<XGI_SESSION_DETAILS>(buffer_ptr, buffer_length);
      *result = d ? SessionGetDetails(*d) : kEInvalidArg;
      return true;
    }
    case 0x000B001E: {
      auto* d = MessageData<XGI_SESSION_MIGRATE>(buffer_ptr, buffer_length);
      *result = d ? SessionMigrate(*d) : kEInvalidArg;
      return true;
    }
    case 0x000B0021: {
      auto* d = MessageData<XGI_XUSER_READ_STATS>(buffer_ptr, buffer_length);
      *result = d ? UserReadStats(*d) : kEInvalidArg;
      return true;
    }
    case 0x000B0025:
    case 0x000B0026: {
      auto* d = MessageData<XGI_STATS_WRITE>(buffer_ptr, buffer_length);
      *result = d ? SessionWriteStats(*d, message == 0x000B0026) : kEInvalidArg;
      return true;
    }
    default:
      return false;  // achievements and the rest: the SDK's XGI app
  }
}

bool Handle(uint32_t app, uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length,
            uint32_t* result) {
  if (!OnlineState::Get().enabled()) {
    return false;
  }
  if (app == kAppXgi) {
    return HandleXgi(message, buffer_ptr, buffer_length, result);
  }
  if (app == kAppXLiveBase) {
    return HandleXLiveBase(message, buffer_ptr, result);
  }
  return false;
}

uint32_t StartIORequest(uint32_t app, uint32_t message, uint32_t overlapped_ptr,
                        uint32_t buffer_ptr, uint32_t buffer_length) {
  uint32_t result;
  if (!Handle(app, message, buffer_ptr, buffer_length, &result)) {
    result = KS()->app_manager()->DispatchMessageAsync(app, message, buffer_ptr, buffer_length);
    if (result == kENotFound) {
      ONLINE_WARN("XMsgStartIORequest: app {:#x} message {:#x} is not known", app, message);
      result = kEInvalidArg;
      sys::XThread::SetLastError(kErrorNotFound);
    }
  }
  if (overlapped_ptr) {
    KS()->CompleteOverlappedImmediate(overlapped_ptr, result);
    result = kErrorIoPending;
  }
  if (result == kESuccess || result == kErrorIoPending) {
    sys::XThread::SetLastError(0);
  }
  return result;
}

u32 XMsgInProcessCall_entry(u32 app, u32 message, u32 arg1, u32 arg2) {
  uint32_t result;
  if (!Handle(app, message, arg1, arg2, &result)) {
    result = KS()->app_manager()->DispatchMessageSync(app, message, arg1, arg2);
  }
  return result;
}

u32 XMsgStartIORequest_entry(u32 app, u32 message, ppc_ptr_t<sys::XAM_OVERLAPPED> overlapped,
                             u32 buffer_ptr, u32 buffer_length) {
  return StartIORequest(app, message, overlapped.guest_address(), buffer_ptr, buffer_length);
}

u32 XMsgStartIORequestEx_entry(u32 app, u32 message, ppc_ptr_t<sys::XAM_OVERLAPPED> overlapped,
                               u32 buffer_ptr, u32 buffer_length, mapped_void unknown) {
  return StartIORequest(app, message, overlapped.guest_address(), buffer_ptr, buffer_length);
}

// --- Session handles -------------------------------------------------------------

u32 XamSessionCreateHandle_entry(mapped_u32 handle_ptr) {
  if (!handle_ptr) {
    return kEInvalidArg;
  }
  std::lock_guard<std::mutex> lock(g_sessions_mutex);
  auto* session = new SessionObject(KS());
  *handle_ptr = session->handle();
  ONLINE_TRACE("XamSessionCreateHandle -> {:08X}", session->handle());
  return kESuccess;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 obj_ptr) {
  if (!obj_ptr) {
    return kEInvalidArg;
  }
  std::lock_guard<std::mutex> lock(g_sessions_mutex);
  auto object = KS()->object_table()->LookupObject<sys::XObject>(handle);
  if (!object || object->type() != sys::XObject::Type::Session) {
    ONLINE_WARN("XamSessionRefObjByHandle({:08X}): not a session", handle);
    return kErrorInvalidHandle;
  }
  // The game closes the handle it was given right after this; the session
  // lives on through this reference until XSessionDelete.
  object->RetainHandle();
  *obj_ptr = object->guest_object();
  return kESuccess;
}

}  // namespace

REX_HOOK(__imp__XMsgInProcessCall, XMsgInProcessCall_entry)
REX_HOOK(__imp__XMsgStartIORequest, XMsgStartIORequest_entry)
REX_HOOK(__imp__XMsgStartIORequestEx, XMsgStartIORequestEx_entry)
REX_HOOK(__imp__XamSessionCreateHandle, XamSessionCreateHandle_entry)
REX_HOOK(__imp__XamSessionRefObjByHandle, XamSessionRefObjByHandle_entry)

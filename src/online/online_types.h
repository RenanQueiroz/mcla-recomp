// Guest-side structures of the Xbox 360 networking and Live APIs the game uses,
// laid out as the game reads them (big-endian, packed as the XDK packs them).
// The names follow the XDK and Xenia's netplay fork; the sizes are asserted so
// a wrong layout fails to compile rather than corrupting the game's memory.

#pragma once

#include <cstdint>
#include <cstring>

#include <rex/types.h>

namespace mcla::online {

using rex::be;

// --- Addresses and keys ------------------------------------------------------

// Four bytes of an IPv4 address in network byte order, as in_addr holds them.
struct InAddr {
  uint32_t raw = 0;  // network byte order: first byte of the address first

  uint32_t host_order() const {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&raw);
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
  }
  static InAddr from_host_order(uint32_t v) {
    InAddr a;
    uint8_t* b = reinterpret_cast<uint8_t*>(&a.raw);
    b[0] = uint8_t(v >> 24);
    b[1] = uint8_t(v >> 16);
    b[2] = uint8_t(v >> 8);
    b[3] = uint8_t(v);
    return a;
  }
  bool operator==(const InAddr& o) const { return raw == o.raw; }
};
static_assert(sizeof(InAddr) == 4);

struct XNKID {
  uint8_t ab[8];
};
static_assert(sizeof(XNKID) == 8);

struct XNKEY {
  uint8_t ab[16];
};
static_assert(sizeof(XNKEY) == 16);

// Security gateway address: the "online identification" of an XNADDR.
#pragma pack(push, 4)
struct SGADDR {
  InAddr ina;
  be<uint32_t> security_parameter_index;
  be<uint64_t> xbox_id;
  uint8_t platform_type;
  uint8_t reserved[3];
};
#pragma pack(pop)
static_assert(sizeof(SGADDR) == 0x14);

struct XNADDR {
  InAddr ina;             // LAN address
  InAddr ina_online;      // public address
  be<uint16_t> port_online;
  uint8_t enet[6];        // MAC address: what tells consoles apart
  SGADDR online;
};
static_assert(sizeof(XNADDR) == 0x24);

struct XSESSION_INFO {
  XNKID session_id;
  XNADDR host_address;
  XNKEY key_exchange_key;
};
static_assert(sizeof(XSESSION_INFO) == 0x3C);

// --- Sessions ----------------------------------------------------------------

struct XUSER_CONTEXT {
  be<uint32_t> context_id;
  be<uint32_t> value;
};
static_assert(sizeof(XUSER_CONTEXT) == 8);

// XUSER_DATA: the type byte, then an 8-byte aligned union.
struct X_USER_DATA {
  uint8_t type;
  uint8_t pad[3];
  be<uint32_t> unk_4;
  union {
    be<int32_t> s32;
    be<int64_t> s64;
    be<double> f64;
    be<float> f32;
    struct {
      be<uint32_t> size;
      be<uint32_t> ptr;
    } blob;  // unicode and binary data
    be<uint64_t> filetime;
    uint8_t raw[8];
  };
};
static_assert(sizeof(X_USER_DATA) == 16);

struct XUSER_PROPERTY {
  be<uint32_t> property_id;
  be<uint32_t> pad;
  X_USER_DATA data;
};
static_assert(sizeof(XUSER_PROPERTY) == 0x18);

enum UserDataType : uint8_t {
  kDataContext = 0,
  kDataInt32 = 1,
  kDataInt64 = 2,
  kDataDouble = 3,
  kDataWString = 4,
  kDataFloat = 5,
  kDataBinary = 6,
  kDataDateTime = 7,
};

inline uint8_t PropertyType(uint32_t property_id) {
  return uint8_t(property_id >> 28);
}

struct XSESSION_SEARCHRESULT {
  XSESSION_INFO info;
  be<uint32_t> open_public_slots;
  be<uint32_t> open_private_slots;
  be<uint32_t> filled_public_slots;
  be<uint32_t> filled_private_slots;
  be<uint32_t> properties_count;
  be<uint32_t> contexts_count;
  be<uint32_t> properties_ptr;
  be<uint32_t> contexts_ptr;
};
static_assert(sizeof(XSESSION_SEARCHRESULT) == 0x5C);

struct XSESSION_SEARCHRESULT_HEADER {
  be<uint32_t> search_results_count;
  be<uint32_t> search_results_ptr;
};
static_assert(sizeof(XSESSION_SEARCHRESULT_HEADER) == 8);

enum SessionState : uint32_t {
  kStateLobby = 0,
  kStateRegistration = 1,
  kStateInGame = 2,
  kStateReporting = 3,
  kStateDeleted = 4,
};

struct XSESSION_MEMBER {
  be<uint64_t> online_xuid;
  be<uint32_t> user_index;
  be<uint32_t> flags;  // 1: private slot, 2: zombie
};
static_assert(sizeof(XSESSION_MEMBER) == 0x10);

struct XSESSION_LOCAL_DETAILS {
  be<uint32_t> user_index_host;
  be<uint32_t> game_type;
  be<uint32_t> game_mode;
  be<uint32_t> flags;
  be<uint32_t> max_public_slots;
  be<uint32_t> max_private_slots;
  be<uint32_t> available_public_slots;
  be<uint32_t> available_private_slots;
  be<uint32_t> actual_member_count;
  be<uint32_t> returned_member_count;
  be<uint32_t> state;
  be<uint64_t> nonce;
  XSESSION_INFO session_info;
  XNKID xnkid_arbitration;
  be<uint32_t> session_members_ptr;
};
static_assert(sizeof(XSESSION_LOCAL_DETAILS) == 0x80);

struct XSESSION_REGISTRANT {
  be<uint64_t> machine_id;
  be<uint32_t> trustworthiness;
  be<uint32_t> num_users;
  be<uint32_t> users_ptr;
};
static_assert(sizeof(XSESSION_REGISTRANT) == 0x18);

struct XSESSION_REGISTRATION_RESULTS {
  be<uint32_t> registrants_count;
  be<uint32_t> registrants_ptr;
};
static_assert(sizeof(XSESSION_REGISTRATION_RESULTS) == 8);

// Session flags (XSESSION_CREATE_*).
enum SessionFlags : uint32_t {
  kSessionHost = 0x01,
  kSessionPresence = 0x02,
  kSessionStats = 0x04,
  kSessionMatchmaking = 0x08,
  kSessionArbitration = 0x10,
  kSessionPeerNetwork = 0x20,
  kSessionSocialMatchmakingAllowed = 0x80,
  kSessionInvitesDisabled = 0x100,
  kSessionJoinViaPresenceDisabled = 0x200,
  kSessionJoinInProgressDisabled = 0x400,
  kSessionJoinViaPresenceFriendsOnly = 0x800,
};

// The guest object XamSessionRefObjByHandle hands out; the XGI messages name a
// session by its address.
struct X_KSESSION {
  be<uint32_t> handle;
};

// --- XGI (app 0xFB) message buffers --------------------------------------------

struct XGI_XUSER_SET_CONTEXT {
  be<uint32_t> user_index;
  be<uint32_t> unused;
  be<uint64_t> xuid;
  XUSER_CONTEXT context;
};
static_assert(sizeof(XGI_XUSER_SET_CONTEXT) == 0x18);

struct XGI_XUSER_SET_PROPERTY {
  be<uint32_t> user_index;
  be<uint32_t> unused;
  be<uint64_t> xuid;
  be<uint32_t> property_id;
  be<uint32_t> data_size;
  be<uint32_t> data_address;
};
static_assert(sizeof(XGI_XUSER_SET_PROPERTY) == 0x20);

struct XGI_SESSION_CREATE {
  be<uint32_t> obj_ptr;
  be<uint32_t> flags;
  be<uint32_t> num_slots_public;
  be<uint32_t> num_slots_private;
  be<uint32_t> user_index;
  be<uint32_t> session_info_ptr;
  be<uint32_t> nonce_ptr;
};
static_assert(sizeof(XGI_SESSION_CREATE) == 0x1C);

struct XGI_SESSION_STATE {
  be<uint32_t> obj_ptr;
  be<uint32_t> flags;
  be<uint64_t> session_nonce;
};
static_assert(sizeof(XGI_SESSION_STATE) == 0x10);

struct XGI_SESSION_MANAGE {
  be<uint32_t> obj_ptr;
  be<uint32_t> array_count;
  be<uint32_t> xuid_array_ptr;
  be<uint32_t> indices_array_ptr;
  be<uint32_t> private_slots_array_ptr;
};
static_assert(sizeof(XGI_SESSION_MANAGE) == 0x14);

struct XGI_SESSION_MODIFY {
  be<uint32_t> obj_ptr;
  be<uint32_t> flags;
  be<uint32_t> max_public_slots;
  be<uint32_t> max_private_slots;
};
static_assert(sizeof(XGI_SESSION_MODIFY) == 0x10);

struct XGI_SESSION_SEARCH {
  be<uint32_t> proc_index;
  be<uint32_t> user_index;
  be<uint32_t> num_results;
  be<uint16_t> num_props;
  be<uint16_t> num_ctx;
  be<uint32_t> props_ptr;
  be<uint32_t> ctx_ptr;
  be<uint32_t> results_buffer_size;
  be<uint32_t> search_results_ptr;
};
static_assert(sizeof(XGI_SESSION_SEARCH) == 0x20);

struct XGI_SESSION_SEARCH_EX {
  XGI_SESSION_SEARCH search;
  be<uint32_t> num_users;
};
static_assert(sizeof(XGI_SESSION_SEARCH_EX) == 0x24);

struct XGI_SESSION_DETAILS {
  be<uint32_t> obj_ptr;
  be<uint32_t> details_buffer_size;
  be<uint32_t> session_details_ptr;
  be<uint32_t> reserved[3];
};
static_assert(sizeof(XGI_SESSION_DETAILS) == 0x18);

struct XGI_SESSION_MIGRATE {
  be<uint32_t> obj_ptr;
  be<uint32_t> session_info_ptr;
  be<uint32_t> user_index;
  be<uint32_t> reserved[3];
};
static_assert(sizeof(XGI_SESSION_MIGRATE) == 0x18);

struct XGI_SESSION_ARBITRATION {
  be<uint32_t> obj_ptr;
  be<uint32_t> flags;
  be<uint64_t> session_nonce;
  be<uint32_t> session_duration_sec;
  be<uint32_t> results_buffer_size;
  be<uint32_t> results_ptr;
};
static_assert(sizeof(XGI_SESSION_ARBITRATION) == 0x20);

struct XGI_XUSER_READ_STATS {
  be<uint32_t> title_id;
  be<uint32_t> xuids_count;
  be<uint32_t> xuids_ptr;
  be<uint32_t> specs_count;
  be<uint32_t> specs_ptr;
  be<uint32_t> results_size;
  be<uint32_t> results_ptr;
};
static_assert(sizeof(XGI_XUSER_READ_STATS) == 0x1C);

struct XGI_STATS_WRITE {
  be<uint32_t> obj_ptr;
  be<uint64_t> xuid;
  be<uint32_t> num_views;
  be<uint32_t> views_ptr;
};
static_assert(sizeof(XGI_STATS_WRITE) == 0x18);

struct XSESSION_VIEW_PROPERTIES {
  be<uint32_t> view_id;
  be<uint32_t> properties_count;
  be<uint32_t> properties_ptr;
};
static_assert(sizeof(XSESSION_VIEW_PROPERTIES) == 0xC);

struct X_USER_STATS_SPEC {
  be<uint32_t> view_id;
  be<uint32_t> num_column_ids;
  be<uint16_t> column_ids[64];
};
static_assert(sizeof(X_USER_STATS_SPEC) == 8 + 64 * 2);

struct X_USER_STATS_VIEW {
  be<uint32_t> view_id;
  be<uint32_t> total_view_rows;
  be<uint32_t> num_rows;
  be<uint32_t> rows_ptr;
};
static_assert(sizeof(X_USER_STATS_VIEW) == 0x10);

struct X_USER_STATS_READ_RESULTS {
  be<uint32_t> num_views;
  be<uint32_t> views_ptr;
};
static_assert(sizeof(X_USER_STATS_READ_RESULTS) == 8);

// --- QoS ----------------------------------------------------------------------

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  be<uint16_t> probes_xmit;
  be<uint16_t> probes_recv;
  be<uint16_t> data_len;
  be<uint32_t> data_ptr;
  be<uint16_t> rtt_min_ms;
  be<uint16_t> rtt_med_ms;
  be<uint32_t> up_bits_per_sec;
  be<uint32_t> down_bits_per_sec;
};
static_assert(sizeof(XNQOSINFO) == 0x18);

struct XNQOS {
  be<uint32_t> count;
  be<uint32_t> count_pending;
  XNQOSINFO info[1];
};

enum QosInfoFlags : uint8_t {
  kQosComplete = 0x01,
  kQosTargetContacted = 0x02,
  kQosTargetDisabled = 0x04,
  kQosDataReceived = 0x08,
  kQosPartialComplete = 0x10,
};

enum QosListenFlags : uint32_t {
  kQosListenEnable = 0x01,
  kQosListenDisable = 0x02,
  kQosListenSetData = 0x04,
  kQosListenSetBitsPerSec = 0x08,
  kQosListenRelease = 0x10,
};

// --- Users ----------------------------------------------------------------------

struct X_USER_SIGNIN_INFO {
  be<uint64_t> xuid;
  be<uint32_t> flags;
  be<uint32_t> signin_state;
  be<uint32_t> guest_number;
  be<uint32_t> sponsor_user_index;
  char name[16];
};
static_assert(sizeof(X_USER_SIGNIN_INFO) == 40);

enum : uint32_t {
  kSigninStateNotSignedIn = 0,
  kSigninStateSignedInLocally = 1,
  kSigninStateSignedInToLive = 2,

  kUserInfoFlagLiveEnabled = 0x1,
  kUserInfoFlagGuest = 0x2,

  kGetSigninInfoOnlineXuidOnly = 0x1,
  kGetSigninInfoOfflineXuidOnly = 0x2,

  kXuidMaskOffline = 0x1,
  kXuidMaskOnline = 0x2,
  kXuidMaskGuest = 0x4,
};

// --- Sockets and XNet ------------------------------------------------------------

struct XSOCKADDR_IN {
  be<uint16_t> sin_family;
  be<uint16_t> sin_port;
  InAddr sin_addr;
  char sin_zero[8];
};
static_assert(sizeof(XSOCKADDR_IN) == 16);

enum XnAddrStatus : uint32_t {
  kXnAddrPending = 0x0000,
  kXnAddrNone = 0x0001,
  kXnAddrEthernet = 0x0002,
  kXnAddrStatic = 0x0004,
  kXnAddrDhcp = 0x0008,
  kXnAddrPppoe = 0x0010,
  kXnAddrGateway = 0x0020,
  kXnAddrDns = 0x0040,
  kXnAddrOnline = 0x0080,
  kXnAddrTroubleshoot = 0x8000,
};

enum EthernetLinkStatus : uint32_t {
  kEthernetLinkActive = 0x01,
  kEthernetLink100Mbps = 0x02,
  kEthernetLink10Mbps = 0x04,
  kEthernetLinkFullDuplex = 0x08,
  kEthernetLinkHalfDuplex = 0x10,
  kEthernetLinkWireless = 0x20,
};

enum XnetConnectStatus : uint32_t {
  kXnetConnectIdle = 0,
  kXnetConnectPending = 1,
  kXnetConnectConnected = 2,
  kXnetConnectLost = 3,
};

// Winsock error codes as the game sees them.
enum : uint32_t {
  kWsaEInval = 10022,
  kWsaEWouldBlock = 10035,
  kWsaENotSock = 10038,
  kWsaEHostUnreach = 10065,
  kWsaNotInitialised = 10093,
};

// Notifications.
enum : uint32_t {
  kNotifySysUi = 0x00000009,
  kNotifySysSigninChanged = 0x0000000A,
  kNotifyLiveConnectionChanged = 0x02000001,
  kNotifyLiveLinkStateChanged = 0x02000003,
  kOnlineLogonConnectionEstablished = 0x001510F0,
  kOnlineLogonDisconnected = 0x001510F1,
};

// Online HRESULTs.
enum : uint32_t {
  kOnlineESessionNotFound = 0x80155200,
  kOnlineESessionFull = 0x80155202,
  kOnlineESessionWrongState = 0x80155206,
  kOnlineESessionInsufficientBuffer = 0x80155207,
  kOnlineESessionNotLoggedOn = 0x80155209,
  kOnlineELogonNotLoggedOn = 0x80151802,
};

// --- Helpers ----------------------------------------------------------------

inline uint64_t XnkidToU64(const XNKID& id) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v = (v << 8) | id.ab[i];
  }
  return v;
}

inline XNKID U64ToXnkid(uint64_t v) {
  XNKID id{};
  for (int i = 7; i >= 0; --i) {
    id.ab[i] = uint8_t(v);
    v >>= 8;
  }
  return id;
}

}  // namespace mcla::online

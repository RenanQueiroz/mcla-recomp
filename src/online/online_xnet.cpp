// NetDll_XNet*: the secure-address layer of Xbox LIVE, answered with real
// addresses. Consoles are told apart by their XNADDR (MAC address, LAN and
// public address); XNetXnAddrToInAddr gives the game the address that reaches
// a peer, and the game's own UDP sockets (the SDK's, real sockets) carry the
// race. QoS answers come from the private server, which keeps the data each
// host publishes with XNetQosListen.

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xevent.h>
#include <rex/system/xthread.h>

#include "online.h"
#include "online_client.h"
#include "online_util.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mcla::online {

// QoS data learnt from search results, so a lookup that follows a search needs
// no second round trip.
namespace {
std::mutex g_qos_mutex;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_qos_cache;
}  // namespace

void CacheQosData(uint64_t session_id, std::vector<uint8_t> data) {
  std::lock_guard<std::mutex> lock(g_qos_mutex);
  g_qos_cache[session_id] = std::move(data);
}

static bool LookupQosData(uint64_t session_id, std::vector<uint8_t>* out) {
  {
    std::lock_guard<std::mutex> lock(g_qos_mutex);
    auto it = g_qos_cache.find(session_id);
    if (it != g_qos_cache.end()) {
      *out = it->second;
      return true;
    }
  }
  if (OnlineState::Get().connected() && OnlineState::Get().server().GetQos(session_id, out)) {
    CacheQosData(session_id, *out);
    return true;
  }
  return false;
}

// The SDK's own definition of an export this module replaces, for the ones
// that are wrapped rather than rewritten.
using ExportFunc = void (*)(PPCContext&, uint8_t*);
static ExportFunc OriginalExport(const char* name) {
#ifdef _WIN32
  HMODULE module = GetModuleHandleW(L"rexruntime.dll");
  if (!module) {
    return nullptr;
  }
  return reinterpret_cast<ExportFunc>(GetProcAddress(module, name));
#else
  (void)name;
  return nullptr;
#endif
}

}  // namespace mcla::online

namespace {

using namespace mcla::online;
namespace sys = rex::system;

struct XNetStartupParams {
  uint8_t size_of_struct;
  uint8_t flags;
  uint8_t max_dgram_sockets;
  uint8_t max_stream_sockets;
  uint8_t default_recv_bufsize_k;
  uint8_t default_send_bufsize_k;
  uint8_t key_reg_max;
  uint8_t sec_reg_max;
  uint8_t qos_data_limit_div4;
  uint8_t qos_probe_timeout_s;
  uint8_t qos_probe_retries;
  uint8_t qos_srv_max_simultaneous_responses;
  uint8_t qos_pair_wait_time_s;
};

u32 XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    ONLINE_TRACE("XNetStartup(flags {:#x}, {} datagram sockets, QoS data limit {} bytes)",
                 params->flags, params->max_dgram_sockets, params->qos_data_limit_div4 * 4);
  } else {
    ONLINE_TRACE("XNetStartup()");
  }
  return 0;
}

u32 XNetCleanup_entry(u32 caller, mapped_void params) {
  ONLINE_TRACE("XNetCleanup()");
  return 0;
}

u32 XNetGetEthernetLinkStatus_entry(u32 caller) {
  if (!OnlineState::Get().enabled()) {
    return 0;
  }
  return kEthernetLinkActive | kEthernetLink100Mbps | kEthernetLinkFullDuplex;
}

u32 XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr) {
  if (!addr) {
    return kXnAddrNone;
  }
  auto& state = OnlineState::Get();
  if (!state.enabled()) {
    // As the SDK answered: a console on its own with a loopback address.
    std::memset(addr, 0, sizeof(XNADDR));
    addr->ina = InAddr::from_host_order(0x7F000001u);
    std::memset(addr->enet, 0xCC, 6);
    return kXnAddrStatic;
  }
  state.FillOurXnAddr(addr);
  uint32_t status = kXnAddrEthernet | kXnAddrStatic | kXnAddrGateway | kXnAddrDns;
  if (state.connected()) {
    status |= kXnAddrOnline;
  }
  // The game asks every frame: logged when the answer changes.
  static std::mutex last_mutex;
  static XNADDR last_addr{};
  static uint32_t last_status = ~0u;
  {
    std::lock_guard<std::mutex> lock(last_mutex);
    if (status != last_status || std::memcmp(&last_addr, addr, sizeof(XNADDR)) != 0) {
      last_status = status;
      last_addr = *addr;
      ONLINE_TRACE("XNetGetTitleXnAddr -> {} / {} port {} status {:#x}",
                   Ipv4ToString(addr->ina.host_order()),
                   Ipv4ToString(addr->ina_online.host_order()), uint16_t(addr->port_online),
                   status);
    }
  }
  return status;
}

u32 XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xnaddr, ppc_ptr_t<XNKID> xnkid,
                             ppc_ptr_t<InAddr> ina) {
  if (!xnaddr || !ina) {
    return kWsaEInval;
  }
  const uint32_t ip = OnlineState::Get().ResolveXnAddr(*xnaddr);
  if (!ip) {
    ONLINE_WARN("XNetXnAddrToInAddr: an XNADDR without an address (MAC {})",
                HexBytes(xnaddr->enet, 6));
    return kWsaEInval;
  }
  *ina = InAddr::from_host_order(ip);
  ONLINE_TRACE("XNetXnAddrToInAddr(MAC {}, {} / {}) -> {}", HexBytes(xnaddr->enet, 6),
               Ipv4ToString(xnaddr->ina.host_order()),
               Ipv4ToString(xnaddr->ina_online.host_order()), Ipv4ToString(ip));
  return 0;
}

u32 XNetUnregisterInAddr_entry(u32 caller, u32 ina) {
  ONLINE_TRACE("XNetUnregisterInAddr({})", Ipv4ToString(InAddr{ina}.host_order()));
  return 0;
}

u32 XNetRegisterKey_entry(u32 caller, ppc_ptr_t<XNKID> xnkid, ppc_ptr_t<XNKEY> xnkey) {
  if (xnkid) {
    ONLINE_TRACE("XNetRegisterKey({})", Hex64(XnkidToU64(*xnkid)));
  }
  return 0;
}

u32 XNetUnregisterKey_entry(u32 caller, ppc_ptr_t<XNKID> xnkid) {
  if (xnkid) {
    ONLINE_TRACE("XNetUnregisterKey({})", Hex64(XnkidToU64(*xnkid)));
  }
  return 0;
}

u32 XNetQosListen_entry(u32 caller, ppc_ptr_t<XNKID> xnkid, mapped_void data, u32 data_size,
                        u32 bits_per_sec, u32 flags) {
  if (!xnkid) {
    return kWsaEInval;
  }
  const uint64_t session_id = XnkidToU64(*xnkid);
  ONLINE_TRACE("XNetQosListen({}, {} bytes, {} bit/s, flags {:#x})", Hex64(session_id),
               data_size, bits_per_sec, flags);
  if ((flags & kQosListenSetData) && data && data_size) {
    auto& state = OnlineState::Get();
    std::vector<uint8_t> bytes(static_cast<const uint8_t*>(data.host_address()),
                               static_cast<const uint8_t*>(data.host_address()) + data_size);
    CacheQosData(session_id, bytes);
    if (state.connected()) {
      state.server().SetQos(session_id, bytes.data(), bytes.size());
    }
  }
  return 0;
}

// XNetQosLookup(cxnqos, apxna, apxnkid, apxnkey, cina, aina, adwServiceId,
//               cProbes, dwBitsPerSec, dwFlags, hEvent, ppxnqos)
u32 XNetQosLookup_entry(u32 caller, u32 count, mapped_u32 xnaddr_ptrs, mapped_u32 xnkid_ptrs,
                        mapped_u32 xnkey_ptrs, u32 gateway_count, mapped_void gateways,
                        mapped_u32 service_ids, u32 probes, u32 bits_per_sec, u32 flags,
                        u32 event_handle, mapped_u32 qos_out) {
  if (!qos_out) {
    return kWsaEInval;
  }
  auto* ks = sys::kernel_state();
  auto* memory = ks->memory();
  const uint32_t total = count + gateway_count;
  const uint32_t size = uint32_t(sizeof(XNQOS) + (total ? total - 1 : 0) * sizeof(XNQOSINFO));
  const uint32_t qos_addr = memory->SystemHeapAlloc(size);
  if (!qos_addr) {
    return kWsaEInval;
  }
  auto* qos = memory->TranslateVirtual<XNQOS*>(qos_addr);
  std::memset(qos, 0, size);
  qos->count = total;
  qos->count_pending = 0;

  for (uint32_t i = 0; i < total; ++i) {
    XNQOSINFO& info = qos->info[i];
    info.probes_xmit = uint16_t(probes ? probes : 1);
    info.probes_recv = info.probes_xmit;
    info.rtt_min_ms = 20;
    info.rtt_med_ms = 30;
    info.up_bits_per_sec = 2000000;
    info.down_bits_per_sec = 2000000;
    info.flags = kQosComplete | kQosTargetContacted;
    if (i >= count || !xnkid_ptrs) {
      continue;
    }
    const uint32_t xnkid_ptr = xnkid_ptrs.as_array<uint32_t>()[i];
    if (!xnkid_ptr) {
      continue;
    }
    const uint64_t session_id = XnkidToU64(*memory->TranslateVirtual<XNKID*>(xnkid_ptr));
    std::vector<uint8_t> data;
    if (LookupQosData(session_id, &data) && !data.empty()) {
      const uint32_t data_addr = memory->SystemHeapAlloc(uint32_t(data.size()));
      if (data_addr) {
        std::memcpy(memory->TranslateVirtual<uint8_t*>(data_addr), data.data(), data.size());
        info.data_ptr = data_addr;
        info.data_len = uint16_t(data.size());
        info.flags |= kQosDataReceived;
      }
    }
    ONLINE_TRACE("XNetQosLookup: session {} -> {} bytes of QoS data", Hex64(session_id),
                 data.size());
  }
  *qos_out = qos_addr;

  if (event_handle) {
    auto event = ks->object_table()->LookupObject<sys::XEvent>(event_handle);
    if (event) {
      event->Set(0, false);
    }
  }
  ONLINE_TRACE("XNetQosLookup({} targets, {} gateways, flags {:#x}) -> {:08X}", count,
               gateway_count, flags, qos_addr);
  return 0;
}

u32 XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return kWsaEInval;
  }
  auto* memory = sys::kernel_state()->memory();
  const uint32_t count = qos->count;
  for (uint32_t i = 0; i < count; ++i) {
    if (qos->info[i].data_ptr) {
      memory->SystemHeapFree(qos->info[i].data_ptr);
    }
  }
  memory->SystemHeapFree(qos.guest_address());
  return 0;
}

// The title server: Rockstar's, which is gone. Without an address configured
// the game is told the service is unreachable and goes on without it.
u32 XNetServerToInAddr_entry(u32 caller, u32 server_addr, u32 service_id, ppc_ptr_t<InAddr> ina) {
  const uint32_t ip = OnlineState::Get().title_server_ip();
  ONLINE_TRACE("XNetServerToInAddr(service {:#x}) -> {}", service_id,
               ip ? Ipv4ToString(ip) : "none");
  if (!ip || !ina) {
    return kWsaEHostUnreach;
  }
  *ina = InAddr::from_host_order(ip);
  return 0;
}

u32 XNetConnect_entry(u32 caller, u32 ina) {
  ONLINE_TRACE("XNetConnect({})", Ipv4ToString(InAddr{ina}.host_order()));
  return 0;
}

u32 XNetGetConnectStatus_entry(u32 caller, u32 ina) {
  return kXnetConnectConnected;
}

u32 XNetLogonGetTitleID_entry(u32 caller, mapped_void params) {
  return OnlineState::Get().title_id();
}

// --- Sockets: the SDK's, watched ----------------------------------------------
// The game's UDP socket and the port it binds are what peers send to; the port
// goes into our XNADDR and to the server. The SDK's socket, bind and
// closesocket do the work, called through the DLL's own exports.

std::mutex g_socket_mutex;
std::unordered_map<uint32_t, uint32_t> g_socket_types;  // handle -> type (1 stream, 2 dgram)

}  // namespace

extern "C" REX_FUNC(__imp__NetDll_socket) {
  static const ExportFunc original = OriginalExport("__imp__NetDll_socket");
  const uint32_t type = ctx.r5.u32;
  const uint32_t protocol = ctx.r6.u32;
  if (!original) {
    ONLINE_WARN("NetDll_socket: the SDK's export was not found");
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  original(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  if (handle != 0xFFFFFFFFu) {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    g_socket_types[handle] = type;
  }
  ONLINE_TRACE("socket(type {}, protocol {}) -> {:08X}", type, protocol, handle);
}

extern "C" REX_FUNC(__imp__NetDll_closesocket) {
  static const ExportFunc original = OriginalExport("__imp__NetDll_closesocket");
  const uint32_t handle = ctx.r4.u32;
  {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    g_socket_types.erase(handle);
  }
  if (!original) {
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  original(ctx, base);
}

extern "C" REX_FUNC(__imp__NetDll_bind) {
  static const ExportFunc original = OriginalExport("__imp__NetDll_bind");
  const uint32_t handle = ctx.r4.u32;
  const uint32_t name_ptr = ctx.r5.u32;
  uint16_t port = 0;
  if (name_ptr) {
    const auto* name = sys::kernel_state()->memory()->TranslateVirtual<const XSOCKADDR_IN*>(name_ptr);
    if (uint16_t(name->sin_family) == 2) {
      port = name->sin_port;
    }
  }
  if (!original) {
    ONLINE_WARN("NetDll_bind: the SDK's export was not found");
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  original(ctx, base);
  uint32_t type = 0;
  {
    std::lock_guard<std::mutex> lock(g_socket_mutex);
    auto it = g_socket_types.find(handle);
    if (it != g_socket_types.end()) {
      type = it->second;
    }
  }
  ONLINE_TRACE("bind({:08X}, port {}) -> {} (socket type {})", handle, port, int32_t(ctx.r3.s32),
               type);
  if (ctx.r3.s32 == 0 && port && type == 2) {
    OnlineState::Get().set_game_port(port);
  }
}

// --- The exports -------------------------------------------------------------

REX_HOOK(__imp__NetDll_XNetStartup, XNetStartup_entry)
REX_HOOK(__imp__NetDll_XNetCleanup, XNetCleanup_entry)
REX_HOOK(__imp__NetDll_XNetGetEthernetLinkStatus, XNetGetEthernetLinkStatus_entry)
REX_HOOK(__imp__NetDll_XNetGetTitleXnAddr, XNetGetTitleXnAddr_entry)
REX_HOOK(__imp__NetDll_XNetXnAddrToInAddr, XNetXnAddrToInAddr_entry)
REX_HOOK(__imp__NetDll_XNetUnregisterInAddr, XNetUnregisterInAddr_entry)
REX_HOOK(__imp__NetDll_XNetRegisterKey, XNetRegisterKey_entry)
REX_HOOK(__imp__NetDll_XNetUnregisterKey, XNetUnregisterKey_entry)
REX_HOOK(__imp__NetDll_XNetQosListen, XNetQosListen_entry)
REX_HOOK(__imp__NetDll_XNetQosLookup, XNetQosLookup_entry)
REX_HOOK(__imp__NetDll_XNetQosRelease, XNetQosRelease_entry)
REX_HOOK(__imp__NetDll_XNetServerToInAddr, XNetServerToInAddr_entry)
REX_HOOK(__imp__NetDll_XNetConnect, XNetConnect_entry)
REX_HOOK(__imp__NetDll_XNetGetConnectStatus, XNetGetConnectStatus_entry)
REX_HOOK(__imp__XNetLogonGetTitleID, XNetLogonGetTitleID_entry)

// --- The game's start-up thunks --------------------------------------------------
// The game's own WSAStartup and XNetStartup wrappers first look for the newer
// WSAStartupEx and XNetStartupEx by ordinal (XexGetProcedureAddress) and call
// whichever they get, which in the SDK is a stub. They are replaced by direct
// calls, so that the game always starts its networking the plain way.

REX_EXTERN(__imp__NetDll_WSAStartup);

extern "C" REX_FUNC(sub_8244A258) {
  // (caller, version, wsadata): as the thunk would pass them on.
  __imp__NetDll_WSAStartup(ctx, base);
  ONLINE_TRACE("WSAStartup -> {}", uint32_t(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_8244A490) {
  // (caller, params)
  __imp__NetDll_XNetStartup(ctx, base);
}

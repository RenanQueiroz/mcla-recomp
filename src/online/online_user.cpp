// XamUser*: with the private server reached, the profile is signed in to LIVE
// with an online XUID and every privilege; otherwise the SDK's local sign-in.

#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>

#include "online.h"
#include "online_util.h"

namespace {

using namespace mcla::online;

constexpr uint32_t kESuccess = 0;
constexpr uint32_t kEInvalidArg = 0x80070057;
constexpr uint32_t kENoSuchUser = 0x80070525;
constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorInvalidParameter = 87;
constexpr uint32_t kErrorNoSuchUser = 0x525;
constexpr uint32_t kErrorFunctionFailed = 1627;

bool SignedInToLive() {
  const auto& state = OnlineState::Get();
  return state.enabled() && state.connected();
}

u32 XamUserGetSigninState_entry(u32 user_index) {
  if (user_index != 0) {
    return kSigninStateNotSignedIn;
  }
  return SignedInToLive() ? kSigninStateSignedInToLive : kSigninStateSignedInLocally;
}

i32 XamUserGetSigninInfo_entry(u32 user_index, u32 flags, ppc_ptr_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    return i32(kEInvalidArg);
  }
  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  if (user_index != 0) {
    return i32(kENoSuchUser);
  }
  const auto& state = OnlineState::Get();
  const bool live = SignedInToLive();
  const uint64_t offline = state.identity().xuid_offline
                               ? state.identity().xuid_offline
                               : rex::system::kernel_state()->user_profile()->xuid();
  uint64_t xuid = live ? state.identity().xuid_online : offline;
  if (flags & kGetSigninInfoOfflineXuidOnly) {
    xuid = offline;
  }
  if (flags & kGetSigninInfoOnlineXuidOnly) {
    xuid = live ? state.identity().xuid_online : 0;
  }
  info->xuid = xuid;
  info->flags = live ? kUserInfoFlagLiveEnabled : 0;
  info->signin_state = live ? kSigninStateSignedInToLive : kSigninStateSignedInLocally;
  const std::string name = rex::system::kernel_state()->user_profile()->name();
  std::strncpy(info->name, name.c_str(), sizeof(info->name) - 1);
  ONLINE_TRACE("XamUserGetSigninInfo(flags {:#x}) -> {} {}", flags, Hex64(xuid),
               live ? "LIVE" : "local");
  return i32(kESuccess);
}

i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  if (!xuid_ptr) {
    return i32(kEInvalidArg);
  }
  *xuid_ptr = 0;
  if (user_index >= 4) {
    return i32(kEInvalidArg);
  }
  if (user_index != 0) {
    return i32(kENoSuchUser);
  }
  const auto& state = OnlineState::Get();
  const uint64_t offline = rex::system::kernel_state()->user_profile()->xuid();
  uint64_t xuid = offline;
  if ((type_mask & kXuidMaskOnline) && SignedInToLive()) {
    xuid = state.identity().xuid_online;
  } else if (!(type_mask & kXuidMaskOffline)) {
    // Online or guest only, and the profile is neither.
    return i32(kENoSuchUser);
  }
  *xuid_ptr = xuid;
  return i32(kESuccess);
}

u32 XamUserCheckPrivilege_entry(u32 user_index, u32 privilege, mapped_u32 out_value) {
  if (user_index != 0xFF) {
    if (user_index >= 4) {
      return kErrorInvalidParameter;
    }
    if (user_index != 0) {
      return kErrorNoSuchUser;
    }
  }
  if (out_value) {
    *out_value = SignedInToLive() ? 1 : 0;
  }
  ONLINE_TRACE("XamUserCheckPrivilege({}) -> {}", privilege, SignedInToLive() ? 1 : 0);
  return kErrorSuccess;
}

}  // namespace

REX_HOOK(__imp__XamUserGetSigninState, XamUserGetSigninState_entry)
REX_HOOK(__imp__XamUserGetSigninInfo, XamUserGetSigninInfo_entry)
REX_HOOK(__imp__XamUserGetXUID, XamUserGetXUID_entry)
REX_HOOK(__imp__XamUserCheckPrivilege, XamUserCheckPrivilege_entry)

// Leaderboards are not kept yet: the enumerator cannot be created, which the
// game's wrapper turns into a clean failure (the SDK's stub left the return
// register as it was).
extern "C" REX_FUNC(__imp__XamUserCreateStatsEnumerator) {
  ONLINE_TRACE("XamUserCreateStatsEnumerator: no leaderboards on the private server");
  ctx.r3.u64 = kErrorFunctionFailed;
}

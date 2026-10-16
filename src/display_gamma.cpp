// Display gamma: the console's TV ramp, or an sRGB one for PC monitors.
//
// The game writes gamma-encoded values (1.055 x^(1/2.2) - 0.055, from its
// tonemap) into a plain 8_8_8_8 front buffer and leaves the display transfer to
// the console's gamma ramp: a 256-entry, 10-bit table that the display
// hardware applied, and the SDK's presenter applies here. Direct3D sets it up
// once, when the device is created (sub_82426028): the game's ramp, the
// identity (sub_82427008), goes to D3DDevice_SetGammaRamp (sub_82416000),
// which adjusts it for the display through a table built by the XDK helper
// sub_82426B68 from VdGetCurrentDisplayGamma. Type 1 (sRGB) makes that table
// the identity; type 2 (TV, BT.709) makes it BT.709 OETF(sRGB EOTF(x)). The
// SDK answers 2, as for an Xbox 360 on an HDTV, and on a PC monitor, which
// expects sRGB, that darkens the shadows: 8-bit codes 64, 128 and 192 come out
// as 193, 462 and 741 of 1023 (48, 115 and 185 of 255).
//
// mcla_display_gamma picks the type the helper sees: tv (2, the default) or pc
// (1). The game never sets its ramp again, so a change during play calls
// SetGammaRamp again with the same identity ramp at the start of the next
// swap (D3DDevice_Swap, sub_82419E98, on the thread that renders), and that
// swap sends the new ramp to the GPU with the frame, as a game changing its
// ramp would.

#include "generated/default/mcla_init.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_STRING(mcla_display_gamma, "tv", "MCLA",
                      "Display gamma ramp: tv (the console's for an HDTV, BT.709) or pc "
                      "(sRGB, for PC monitors: brighter shadows)")
    .allowed({"tv", "pc"});

// D3DDevice_Swap: the generated body, called by the override below.
REX_EXTERN(__imp__sub_82419E98);
// The default ramps: the identity as a 256-entry table and as a PWL ramp.
REX_EXTERN(sub_82427008);
REX_EXTERN(sub_82427058);
// D3DDevice_SetGammaRamp and D3DDevice_SetPWLGamma.
REX_EXTERN(sub_82416000);
REX_EXTERN(sub_824160C8);

namespace {

// VdGetCurrentDisplayGamma's display types.
constexpr uint32_t kGammaTypeSrgb = 1;
constexpr uint32_t kGammaTypeTv = 2;

enum Mode : int { kModeUnknown = -1, kModeTv = 0, kModePc = 1 };

// The device's front buffer format (low 6 bits), as the device creation
// stores it; 2_10_10_10 formats (7, 54) use the PWL ramp, others the table.
constexpr uint32_t kDeviceFrontBufferFormat = 13608;
// The ramp as converted for the display: red, green, blue, 256 entries each,
// 16-bit with the 10-bit value in the top bits.
constexpr uint32_t kDeviceGammaRamp = 15004;
constexpr uint32_t kRampBytes = 1536;

std::atomic<int> g_wanted{kModeUnknown};
// The mode the helper last built its table for, and the one last asked of
// SetGammaRamp, so that a call that never reached the helper is not repeated
// every frame.
std::atomic<int> g_applied{kModeUnknown};
std::atomic<int> g_requested{kModeUnknown};
std::atomic<bool> g_logged{false};

uint8_t* Membase() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->virtual_membase() : nullptr;
}

uint32_t Read32(uint8_t* base, uint32_t ea) {
  const uint8_t* p = base + ea;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t Read16(uint8_t* base, uint32_t ea) {
  const uint8_t* p = base + ea;
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void Write32(uint8_t* base, uint32_t ea, uint32_t value) {
  uint8_t* p = base + ea;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

int ModeOf(std::string_view value) {
  return value == "pc" ? kModePc : kModeTv;
}

const char* ModeName(int mode) {
  return mode == kModePc ? "PC monitor (sRGB)" : "TV (BT.709)";
}

// The option's value, kept in an atomic by a change callback: the option is
// set on other threads (the pause menu, the dev pad) than the one rendering.
int WantedMode() {
  static std::once_flag once;
  std::call_once(once, [] {
    rex::cvar::RegisterChangeCallback("mcla_display_gamma",
                                      [](std::string_view, std::string_view value) {
                                        g_wanted.store(ModeOf(value), std::memory_order_relaxed);
                                      });
    g_wanted.store(ModeOf(rex::cvar::GetFlagByName("mcla_display_gamma")),
                   std::memory_order_relaxed);
  });
  return g_wanted.load(std::memory_order_relaxed);
}

bool UsesPwlRamp(uint8_t* base, uint32_t device) {
  const uint32_t format = Read32(base, device + kDeviceFrontBufferFormat) & 0x3F;
  return format == 7 || format == 54;
}

// The ramp the device now sends, at a few points of the red channel.
void LogRamp(uint8_t* base, uint32_t device, int mode) {
  if (UsesPwlRamp(base, device)) {
    REXLOG_INFO("display gamma: {} (PWL ramp)", ModeName(mode));
    return;
  }
  auto at = [&](uint32_t index) {
    return Read16(base, device + kDeviceGammaRamp + 2 * index) >> 6;
  };
  REXLOG_INFO("display gamma: {}; ramp at 0, 64, 128, 192, 255: {}, {}, {}, {}, {} of 1023",
              ModeName(mode), at(0), at(64), at(128), at(192), at(255));
}

// Sets the game's ramp again, as the device creation did: the identity,
// through SetGammaRamp (or SetPWLGamma), which converts it for the display
// type the helper now gets. Called at the start of D3DDevice_Swap, on the
// thread that owns the device, with the ramp on the guest stack below the
// caller's frame.
void SetRampAgain(PPCContext& ctx, uint8_t* base, uint32_t device) {
  const bool pwl = UsesPwlRamp(base, device);
  rex::CallFrame frame(ctx);
  PPCContext& call = frame;
  const uint32_t sp = (ctx.r1.u32 - kRampBytes - 256) & ~UINT32_C(15);
  Write32(base, sp, ctx.r1.u32);  // back chain
  const uint32_t ramp = sp + 128;
  call.r1.u64 = sp;
  call.r3.u64 = ramp;
  call.r4.u64 = 0;
  if (pwl) {
    sub_82427058(call, base);
  } else {
    sub_82427008(call, base);
  }
  call.r1.u64 = sp;
  call.r3.u64 = device;
  call.r4.u64 = 0;
  call.r5.u64 = ramp;
  if (pwl) {
    sub_824160C8(call, base);
  } else {
    sub_82416000(call, base);
  }
}

}  // namespace

// 0x82426B98 in sub_82426B68, right after its call to VdGetCurrentDisplayGamma,
// which wrote the display type at r1+84 (and the power at r1+80).
void mcla_display_gamma_type(PPCRegister& r1) {
  uint8_t* base = Membase();
  if (!base) {
    return;
  }
  const int mode = WantedMode();
  Write32(base, r1.u32 + 84, mode == kModePc ? kGammaTypeSrgb : kGammaTypeTv);
  g_applied.store(mode, std::memory_order_relaxed);
}

// D3DDevice_Swap (r3 = the device).
extern "C" REX_FUNC(sub_82419E98) {
  const uint32_t device = ctx.r3.u32;
  if (device) {
    const int wanted = WantedMode();
    const int applied = g_applied.load(std::memory_order_relaxed);
    if (applied == wanted) {
      g_requested.store(kModeUnknown, std::memory_order_relaxed);
      if (!g_logged.exchange(true, std::memory_order_relaxed)) {
        LogRamp(base, device, applied);  // the ramp the device was created with
      }
    } else if (g_requested.exchange(wanted, std::memory_order_relaxed) != wanted) {
      SetRampAgain(ctx, base, device);
      LogRamp(base, device, wanted);
      g_logged.store(true, std::memory_order_relaxed);
    }
  }
  __imp__sub_82419E98(ctx, base);
}

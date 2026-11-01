#include "generated/default/mcla_init.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/ui/window.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(mcla_aspect_ratio, "auto", "MCLA",
                      "Display aspect: auto (monitor in fullscreen, window otherwise), 16:9 or 43:18")
    .allowed({"auto", "16:9", "43:18"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_hud_aspect, true, "MCLA",
                    "Keep the HUD proportional in a centered 16:9 safe area on an ultrawide display");
REXCVAR_DEFINE_BOOL(mcla_hud_edges, true, "MCLA",
                    "Anchor proportional left/right HUD groups to ultrawide screen edges; keep center prompts centered");
REXCVAR_DEFINE_BOOL(mcla_gps_full_width, true, "MCLA",
                    "Draw GPS movies without the driving HUD safe-area transform");
REXCVAR_DEFINE_BOOL(mcla_world_icon_aspect, true, "MCLA",
                    "Keep world-marker movies and native draws outside the HUD safe-area compression");
REXCVAR_DEFINE_BOOL(mcla_panel_aspect, true, "MCLA",
                    "Keep pause panels outside the HUD safe-area compression");
REXCVAR_DEFINE_BOOL(mcla_phone_aspect, true, "MCLA",
                    "Keep the Sidekick movie outside the HUD safe-area compression");
REXCVAR_DEFINE_BOOL(mcla_garage_aspect, true, "MCLA",
                    "Preserve garage movie proportions in ultrawide");
REXCVAR_DEFINE_BOOL(mcla_gps_icon_aspect, true, "MCLA",
                    "Render the GPS native icon batch and cursor without HUD compression");

namespace {

bool g_wide = false;
double g_hud_correction = 1.0;
thread_local bool g_ui_phase = false;
thread_local uint32_t g_adjusted_device = 0;
thread_local float g_original_scale = 0;
thread_local float g_original_offset = 0;
thread_local float g_hud_anchor_x = 0;
thread_local bool g_gauge_draw = false;
std::atomic<uint32_t> g_speedometer_context{0};
thread_local unsigned g_gps_draw_depth = 0;

bool IsGuestRange(uint32_t address, uint32_t size) {
  return address >= 0x10000 && address < 0xE0000000u &&
      size <= 0xE0000000u - address;
}

template <std::size_t Size>
bool GuestNameEquals(const uint8_t* base, uint32_t address, const char (&name)[Size]) {
  return IsGuestRange(address, uint32_t(Size)) &&
      std::memcmp(base + address, name, Size) == 0;
}

uint32_t ReadWord(const uint8_t* base, uint32_t address) {
  return (uint32_t(base[address]) << 24) | (uint32_t(base[address + 1]) << 16) |
         (uint32_t(base[address + 2]) << 8) | base[address + 3];
}

void WriteWord(uint8_t* base, uint32_t address, uint32_t value) {
  for (unsigned byte = 0; byte < 4; ++byte) {
    base[address + byte] = uint8_t(value >> (24 - 8 * byte));
  }
}

float ReadFloat(const uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(ReadWord(base, address));
}

void WriteFloat(uint8_t* base, uint32_t address, float value) {
  WriteWord(base, address, std::bit_cast<uint32_t>(value));
}

void DirtyViewport(uint8_t* base, uint32_t device) {
  WriteWord(base, device + 20, ReadWord(base, device + 20) | 0x06000000u);
}

void RestoreHudViewport(uint8_t* base) {
  if (!g_adjusted_device) return;
  WriteFloat(base, g_adjusted_device + 10504, g_original_scale);
  WriteFloat(base, g_adjusted_device + 10508, g_original_offset);
  DirtyViewport(base, g_adjusted_device);
  g_adjusted_device = 0;
}

void ApplyHudViewport(uint8_t* base, uint32_t device) {
  if (!g_wide || !g_ui_phase || g_gps_draw_depth || g_gauge_draw ||
      !REXCVAR_GET(mcla_hud_aspect) || device < 0x10000) return;
  const float width = ReadFloat(base, device + 12656);
  const float origin = ReadFloat(base, device + 12648);
  if (std::abs(width - 1280.0f) > 0.1f || std::abs(origin) > 0.1f) return;
  const float correction = float(g_hud_correction);
  g_original_scale = ReadFloat(base, device + 10504);
  g_original_offset = ReadFloat(base, device + 10508);
  g_adjusted_device = device;
  WriteFloat(base, device + 10504, g_original_scale * correction);
  WriteFloat(base, device + 10508,
             640.0f + (g_original_offset - 640.0f) * correction + g_hud_anchor_x);
  DirtyViewport(base, device);
}

uint32_t MovieNameAddress(const uint8_t* base, uint32_t movie) {
  if (!IsGuestRange(movie, 60)) return 0;
  const uint32_t manager = ReadWord(base, 0x8286D804);
  if (!IsGuestRange(manager, 68)) return 0;
  const uint32_t buckets = ReadWord(base, manager + 60);
  const uint32_t count = ReadWord(base, manager + 64) >> 16;
  if (count > 256 || !IsGuestRange(buckets, count * 4)) return 0;
  for (uint32_t bucket = 0; bucket < count; ++bucket) {
    uint32_t node = ReadWord(base, buckets + bucket * 4);
    for (uint32_t guard = 0; IsGuestRange(node, 16) && guard < 256;
         ++guard, node = ReadWord(base, node + 12)) {
      const uint32_t holder = ReadWord(base, node + 8);
      if (!IsGuestRange(holder, 4) || ReadWord(base, holder) != movie) continue;
      return ReadWord(base, node);
    }
  }
  return 0;
}

int HudMovieSide(const uint8_t* base, uint32_t movie) {
  if (!g_wide || !g_ui_phase || !REXCVAR_GET(mcla_hud_aspect) ||
      !REXCVAR_GET(mcla_hud_edges) || !IsGuestRange(movie, 60)) return 0;
  const uint32_t context = ReadWord(base, movie + 56);
  if (context && context == g_speedometer_context.load(std::memory_order_relaxed)) return 1;
  return GuestNameEquals(base, MovieNameAddress(base, movie), "HUDMAPMOVIE") ? -1 : 0;
}

bool IsFullWidthMovie(const uint8_t* base, uint32_t movie) {
  if (!g_wide) return false;
  const uint32_t name = MovieNameAddress(base, movie);
  const bool gps = REXCVAR_GET(mcla_gps_full_width) &&
      (GuestNameEquals(base, name, "NAVSYS2DMOVIE") || GuestNameEquals(base, name, "NAVSYSMOVIE") ||
       GuestNameEquals(base, name, "NAVSYSTARGETICONMOVIE"));
  const bool world_icon = REXCVAR_GET(mcla_world_icon_aspect) &&
      (GuestNameEquals(base, name, "3DICONSMOVIE0") || GuestNameEquals(base, name, "3DICONSMOVIE1") ||
       GuestNameEquals(base, name, "3DICONSMOVIE2"));
  const bool panel = REXCVAR_GET(mcla_panel_aspect) && GuestNameEquals(base, name, "PAUSEMOVIE");
  const bool phone = REXCVAR_GET(mcla_phone_aspect) && GuestNameEquals(base, name, "SIDEKICKMOVIE");
  const bool garage = REXCVAR_GET(mcla_garage_aspect) && GuestNameEquals(base, name, "GARAGEMOVIE");
  return gps || world_icon || panel || phone || garage;
}

template <typename Draw>
void DrawFullWidth(PPCContext& ctx, uint8_t* base, Draw draw, bool full_width) {
  if (full_width) {
    RestoreHudViewport(base);
    ++g_gps_draw_depth;
  }
  draw(ctx, base);
  if (full_width) {
    RestoreHudViewport(base);
    --g_gps_draw_depth;
    ApplyHudViewport(base, ReadWord(base, 0x82839254));
  }
}

template <typename Draw>
void DrawAnchoredHudMovie(PPCContext& ctx, uint8_t* base, Draw draw) {
  const int side = HudMovieSide(base, ctx.r3.u32);
  const float saved_anchor = g_hud_anchor_x;
  const bool saved_gauge = g_gauge_draw;
  g_gauge_draw = side > 0;
  if (side) {
    RestoreHudViewport(base);
    g_hud_anchor_x = side < 0 ? -640.0f * (1.0f - float(g_hud_correction)) : 0.0f;
    ApplyHudViewport(base, ReadWord(base, 0x82839254));
  }
  draw(ctx, base);
  g_gauge_draw = saved_gauge;
  if (side) {
    RestoreHudViewport(base);
    g_hud_anchor_x = saved_anchor;
    ApplyHudViewport(base, ReadWord(base, 0x82839254));
  }
}

struct DisplaySize {
  uint32_t width = 0;
  uint32_t height = 0;
  const char* source = "unavailable";
};

DisplaySize DetectAspectDisplay(rex::ui::Window* window) {
#ifdef _WIN32
  const bool fullscreen = rex::cvar::GetFlagByName("fullscreen") == "true";
  HWND handle = window ? static_cast<HWND>(window->GetNativeWindowHandle()) : nullptr;
  if (!fullscreen && handle) {
    RECT client{};
    if (GetClientRect(handle, &client) && client.right > 0 && client.bottom > 0) {
      return {uint32_t(client.right), uint32_t(client.bottom), "window client"};
    }
  }
  const HMONITOR monitor = handle ? MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST)
      : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info)) {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmPelsWidth && mode.dmPelsHeight) {
      return {mode.dmPelsWidth, mode.dmPelsHeight, "window monitor"};
    }
    return {uint32_t(info.rcMonitor.right - info.rcMonitor.left),
            uint32_t(info.rcMonitor.bottom - info.rcMonitor.top), "monitor bounds"};
  }
#endif
  if (window && window->GetActualPhysicalWidth() && window->GetActualPhysicalHeight()) {
    return {window->GetActualPhysicalWidth(), window->GetActualPhysicalHeight(), "window surface"};
  }
  return {};
}

}

void MclaAspectUiNode(PPCRegister& node, PPCRegister& parent_state) {
  if (!g_gauge_draw) return;
  auto* base = rex::Runtime::instance()->virtual_membase();
  const uint32_t inst = node.u32;
  if (!IsGuestRange(inst, 188) || !IsGuestRange(parent_state.u32, 4)) return;
  const uint32_t parent = ReadWord(base, inst + 184);
  if (!IsGuestRange(parent, 188) || parent_state.u32 != parent + 8) return;
  const uint32_t root = ReadWord(base, parent + 184);
  if (!root) {
    if (std::abs(ReadFloat(base, inst + 48) - 640.0f) > 1.0f ||
        std::abs(ReadFloat(base, inst + 52) - 360.0f) > 1.0f) return;
    const float correction = float(g_hud_correction);
    WriteFloat(base, inst + 32, ReadFloat(base, inst + 32) * correction);
    WriteFloat(base, inst + 40, ReadFloat(base, inst + 40) * correction);
    WriteFloat(base, inst + 48, 640.0f + (ReadFloat(base, inst + 48) - 640.0f) * correction);
    return;
  }
  if (!IsGuestRange(root, 188) || ReadWord(base, root + 184) != 0 ||
      std::abs(ReadFloat(base, parent + 48) - 640.0f) > 1.0f ||
      std::abs(ReadFloat(base, parent + 52) - 360.0f) > 1.0f) return;
  const float local_x = ReadFloat(base, inst + 24);
  const int side = local_x < -256.0f ? -1 : local_x > 256.0f ? 1 : 0;
  WriteFloat(base, inst + 48,
             ReadFloat(base, inst + 48) + side * 640.0f * (1.0f - float(g_hud_correction)));
}

void MclaApplyAspectRatio(rex::ui::Window* window) {
  constexpr uint32_t address = 0x8201E7EC;
  auto* runtime = rex::Runtime::instance();
  auto* memory = runtime ? runtime->memory() : nullptr;
  if (!memory) return;
  auto* heap = memory->LookupHeap(address);
  auto* data = memory->TranslateVirtual<uint8_t*>(address);
  if (!heap || !data) return;
  const uint32_t original = ReadWord(data, 0);
  const std::string selection = REXCVAR_GET(mcla_aspect_ratio);
  DisplaySize detected{};
  uint32_t guest_width = selection == "43:18" ? 1720 : 1280;
  constexpr uint32_t guest_height = 720;
  if (selection == "auto") {
    detected = DetectAspectDisplay(window);
    if (detected.width && detected.height) {
      const double ratio = double(detected.width) / detected.height;
      guest_width = uint32_t(std::clamp(std::lround(ratio * guest_height), 1280L, 4095L));
    } else {
      REXLOG_WARN("automatic aspect: no valid display size; using 16:9");
    }
  }
  double ratio = double(guest_width) / guest_height;
  const uint32_t bits = std::bit_cast<uint32_t>(float(ratio));
  if (original != 0x3FE38E39u) {
    REXLOG_ERROR("aspect ratio: unexpected game constant {:08X}; keeping 16:9", original);
    guest_width = 1280;
    ratio = 16.0 / 9.0;
  } else if (bits != original) {
    uint32_t old_protection = 0;
    if (!heap->Protect(address, 4, rex::memory::kMemoryProtectRead |
                                    rex::memory::kMemoryProtectWrite, &old_protection)) {
      REXLOG_ERROR("aspect ratio: could not write the game constant; keeping 16:9");
      guest_width = 1280;
      ratio = 16.0 / 9.0;
    } else {
      WriteWord(data, 0, bits);
      if (!heap->Protect(address, 4, old_protection)) {
        REXLOG_WARN("aspect ratio: could not restore data page protection");
      }
    }
  }
  g_wide = guest_width > 1280;
  g_hud_correction = 1280.0 / guest_width;
  rex::cvar::SetFlagFromCommandLine("video_mode_width", std::to_string(guest_width));
  rex::cvar::SetFlagFromCommandLine("video_mode_height", "720");
  rex::cvar::SetFlagFromCommandLine("present_letterbox", "true");
  REXLOG_INFO("aspect ratio: selection={} detected={}x{} source={} guest={}x{} ratio={:.6f} HUD={:.6f}",
              selection, detected.width, detected.height, detected.source,
              guest_width, guest_height, ratio, g_hud_correction);
}

void MclaAspectPhase(uint8_t* base, uint32_t phase) {
  RestoreHudViewport(base);
  g_ui_phase = phase == 0x20000000;
  ApplyHudViewport(base, ReadWord(base, 0x82839254));
}

extern "C" REX_FUNC(sub_82721258) {
  DrawFullWidth(ctx, base, __imp__sub_82721258, IsFullWidthMovie(base, ctx.r3.u32));
}

extern "C" REX_FUNC(sub_82241EA0) {
  const uint32_t context = ctx.r4.u32;
  __imp__sub_82241EA0(ctx, base);
  g_speedometer_context.store(context, std::memory_order_relaxed);
}

extern "C" REX_FUNC(sub_82720908) {
  DrawAnchoredHudMovie(ctx, base, __imp__sub_82720908);
}

extern "C" REX_FUNC(sub_82720C58) {
  DrawAnchoredHudMovie(ctx, base, __imp__sub_82720C58);
}

extern "C" REX_FUNC(sub_824162D8) {
  RestoreHudViewport(base);
  const uint32_t device = ctx.r3.u32;
  __imp__sub_824162D8(ctx, base);
  ApplyHudViewport(base, device);
}

extern "C" REX_FUNC(sub_822E5358) {
  RestoreHudViewport(base);
  const bool was_ui = g_ui_phase;
  g_ui_phase = false;
  __imp__sub_822E5358(ctx, base);
  g_ui_phase = was_ui;
  ApplyHudViewport(base, ReadWord(base, 0x82839254));
}

extern "C" REX_FUNC(sub_82413660) {
  if (g_adjusted_device && ctx.r3.u32 == g_adjusted_device && g_ui_phase) {
    const double left = 640.0 + (ctx.r4.s32 - 640.0) * g_hud_correction + g_hud_anchor_x;
    const double right = 640.0 + (ctx.r6.s32 - 640.0) * g_hud_correction + g_hud_anchor_x;
    ctx.r4.s64 = std::clamp(int32_t(std::floor(left + 0.0001)), 0, 1280);
    ctx.r6.s64 = std::clamp(int32_t(std::ceil(right - 0.0001)), 0, 1280);
  }
  __imp__sub_82413660(ctx, base);
}

extern "C" REX_FUNC(sub_82236558) {
  const uint32_t owner = ctx.r3.u32;
  const uint32_t mode = IsGuestRange(owner, 460) ? ReadWord(base, owner + 456) : 0xFFFFFFFFu;
  const bool gps = g_wide && mode == 1 && REXCVAR_GET(mcla_gps_icon_aspect);
  DrawFullWidth(ctx, base, __imp__sub_82236558, gps);
}

extern "C" REX_FUNC(sub_82723940) {
  const bool cursor = g_wide && REXCVAR_GET(mcla_gps_icon_aspect) && ctx.lr == 0x822278D4;
  // World markers already use the camera projection; HUD compression moves them off their cars.
  const bool world = g_wide && REXCVAR_GET(mcla_world_icon_aspect) && ctx.lr == 0x822251F4;
  DrawFullWidth(ctx, base, __imp__sub_82723940, cursor || world);
}
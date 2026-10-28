// Optional detail reductions and a few crash guards. See
// config/perf_options.toml for what each hook address is and where the
// analysis came from (LARecomp, with its author's permission).
//
// Every option defaults to the game as it shipped. They exist because the
// frame rate here is limited by how many draws the GPU emulation gets through
// in a frame, and these are the game's own ways of asking for fewer.

#include "generated/default/mcla_init.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

void MclaFallBackFromFsr();  // src/settings_menu.cpp

REXCVAR_DEFINE_BOOL(mcla_shadows, true, "MCLA", "Real-time shadows. Off is the largest saving");
// The options the game reads once, when it starts, are kRequiresRestart: the
// settings menu marks their rows "(NEXT START)" (src/settings_menu.cpp).
REXCVAR_DEFINE_BOOL(mcla_race_shadows, true, "MCLA",
                    "Shadows during races (applies at the next start of the game)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_fast_car_shadows, false, "MCLA",
                    "A simple blob shadow under cars instead of the real-time one (next start)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_foliage_shadows, true, "MCLA", "Shadows cast by distant foliage");
// The impostor render targets are created with the world, once, at start-up
// (sub_82305230 -> sub_8230C388 -> sub_8230B778, where noimpostors is read):
// starting or quitting a race does not build them again.
REXCVAR_DEFINE_BOOL(mcla_foliage_impostors, true, "MCLA",
                    "Billboards for distant trees (applies at the next start of the game)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_fullscreen_blur, true, "MCLA", "The full-screen blur pass (next start)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Graphics fixes the game applies as defaults of SDK options (MclaApplyGraphicsFixDefaults, below).
REXCVAR_DEFINE_BOOL(mcla_vinyl_readback, true, "MCLA",
                    "Custom vinyls on the opponents' and other cars outside the vinyl editor (fork issues "
                    "11 and 14): the texture the game builds for them (a 512x512 render target with its "
                    "mip chain, drawn when a car loads) is read back into the guest's memory, which "
                    "the game's own code copies it from (the SDK's readback_resolve_rules; used unless that "
                    "option is set itself). Costs nothing outside that moment (next start)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_rain_unscaled_copy, true, "MCLA",
                    "Rain above native resolution (fork issue 17): the game simulates the rain's particles on the GPU "
                    "(four 128x128 two-float render targets, resolved every frame into the memory its vertex shader "
                    "then reads as vertex data). With resolution_scale above 1 the SDK keeps a resolve in its scaled "
                    "buffer and the memory the vertex shader reads stays as the game first wrote it, so no rain is drawn. "
                    "This also writes the 1x version of those four resolves into that memory, on the GPU, with no wait "
                    "(the SDK's resolve_unscaled_copy_rules; used unless that option is set itself), only with "
                    "resolution_scale above 1 (next start)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_vulkan_depth_convert, false, "MCLA",
                    "Vulkan: convert the console's 24-bit float depth in the pixel shader (the SDK's "
                    "depth_float24_convert_in_pixel_shader; used unless that option is set itself). "
                    "An exact float24 depth, off by default: it costs 21% of the frame at 3x on an RTX "
                    "5080 and 33% at 1x on a Radeon iGPU. The blur over a car's plate and rear lights "
                    "(fork issue 11) no longer needs it: the SDK's vulkan_odepth_float24_scale fixes "
                    "it (next start)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(mcla_anti_aliasing, "msaa", "MCLA",
                      "Anti-aliasing:\n"
                      " off: none\n"
                      " msaa: the game's own 2x multisampling (the default)\n"
                      " fsr: AMD FSR 3.1, temporal (no MSAA), at native resolution or, with mcla_upscaling,\n"
                      "       drawing the scene smaller and upscaling it")
    .allowed({"off", "msaa", "fsr"});
REXCVAR_DEFINE_STRING(mcla_upscaling, "off", "MCLA",
                      "FSR upscaling, with mcla_anti_aliasing = fsr: the 3D scene is drawn smaller and FSR 3.1\n"
                      "upscales it, while post-processing, the HUD and the menus stay at the output size:\n"
                      " off: FSR at native resolution (the default)\n"
                      " ultra_quality: 4/3x (3/4 of the width and height: 1080p at resolution_scale 2)\n"
                      " quality: 1.5x (1440p for 4K)\n"
                      " balanced: 1.7x\n"
                      " performance: 2x (1080p for 4K)")
    .allowed({"off", "ultra_quality", "quality", "balanced", "performance"});
REXCVAR_DEFINE_BOOL(mcla_msaa, true, "MCLA",
                    "Before mcla_anti_aliasing: false (no anti-aliasing) is still taken while "
                    "mcla_anti_aliasing isn't set");
REXCVAR_DEFINE_DOUBLE(mcla_city_lod, 1.0, "MCLA",
                      "Scale of the distance at which the city switches to less detail")
    .range(0.1, 4.0);
REXCVAR_DEFINE_DOUBLE(mcla_traffic_lod, 1.0, "MCLA",
                      "Scale of the distance at which traffic switches to less detail")
    .range(0.1, 4.0);
REXCVAR_DEFINE_DOUBLE(mcla_traffic_distance, 400.0, "MCLA",
                      "Distance in metres beyond which ambient traffic is removed (400 as shipped)")
    .range(100.0, 600.0);
REXCVAR_DEFINE_DOUBLE(mcla_pedestrians, 1.0, "MCLA", "Pedestrian density scale").range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(mcla_parked_cars, 1.0, "MCLA", "Parked car density scale").range(0.0, 1.0);
// Off-screen render passes that are drawn only every few frames, the picture of the
// last one kept in between (0 or 1: every frame, as shipped). After the scene itself
// they are what a weak GPU pays most for (launcher/dev/gpu-weak-2026-10-27.md).
REXCVAR_DEFINE_INT32(mcla_far_shadow_interval, 0, "MCLA",
                     "The two farthest sun-shadow cascades are drawn only every this many frames "
                     "(0: every frame, as shipped)")
    .range(0, 8);
REXCVAR_DEFINE_INT32(mcla_env_map_interval, 0, "MCLA",
                     "The environment map of the cars' paint and the speedometer is drawn only "
                     "every this many frames (0: every frame, as shipped)")
    .range(0, 8);
REXCVAR_DEFINE_INT32(mcla_reflection_interval, 0, "MCLA",
                     "The reflection view is drawn only every this many frames (0: every "
                     "frame, as shipped)")
    .range(0, 8);
REXCVAR_DEFINE_STRING(mcla_preset, "custom", "MCLA",
                      "A starting point for the detail options, for a weak GPU:\n"
                      " custom: every option as set (the default)\n"
                      " low: no anti-aliasing and no full-screen blur, the far shadows drawn "
                      "every 2nd frame and the environment map every 4th\n"
                      " lowest: as low, and no real-time shadows\n"
                      "An option set by hand keeps its value")
    .allowed({"custom", "low", "lowest"});

// For measuring what an option is worth. The title screen's camera wanders, so
// two runs never show the same scene; this switches the named option between
// as-shipped and reduced every four seconds within one run and logs frame time
// and draws for each state, which compares like with like.
REXCVAR_DEFINE_STRING(mcla_ab_test, "", "MCLA",
                      "Development: alternate one option every 4 s and log the frame cost of each "
                      "state. shadows, foliage, lod, density or all");

bool MclaSingleTileReadyForMsaaChange();  // src/render_perf.cpp

namespace {

uint8_t* Base() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->virtual_membase() : nullptr;
}

uint32_t Read32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

float ReadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Read32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof value);
  return value;
}

void WriteFloat(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  Write32(base, address, bits);
}

// Anything below the first 64 KiB is not an object.
bool LooksLikePointer(uint32_t value) {
  return value >= 0x10000;
}

// --- Developer switches -------------------------------------------------------

struct DevSwitch {
  const char* name;  // as the game registers it
  bool (*wanted)();
};
const DevSwitch kDevSwitches[] = {
    {"noraceshadows", [] { return !REXCVAR_GET(mcla_race_shadows); }},
    {"fastVehShadows", [] { return bool(REXCVAR_GET(mcla_fast_car_shadows)); }},
    {"noimpostors", [] { return !REXCVAR_GET(mcla_foliage_impostors); }},
    {"nofsblur", [] { return !REXCVAR_GET(mcla_fullscreen_blur); }},
};

// The value every switch gets: a guest string "1", which satisfies both the
// consumers that test the pointer and the ones that parse a number from it.
uint32_t DevSwitchValue() {
  static uint32_t address = 0;
  if (!address) {
    auto* runtime = rex::Runtime::instance();
    if (!runtime || !runtime->memory()) {
      return 0;
    }
    address = runtime->memory()->SystemHeapAlloc(16);
    if (address) {
      uint8_t* text = runtime->virtual_membase() + address;
      std::memset(text, 0, 16);
      text[0] = '1';
    }
  }
  return address;
}

// --- Shadow render phases -------------------------------------------------------

// The renderer object and its mask of enabled render phases, which the frame's
// phase loop reads every frame. The sun-shadow cascades are 0x61E0 (what the
// game's own "noshadows" clears); the shadow drawn at night is phase 0x400.
constexpr uint32_t kRendererPointer = 0x8287E064;
constexpr uint32_t kPhaseEnableMask = 448;
constexpr uint32_t kShadowPhases = 0x65E0;

void ApplyShadowPhases(uint8_t* base) {
  static bool cleared = false;
  static uint32_t were_set = 0;
  const bool want_off = !REXCVAR_GET(mcla_shadows);
  if (!want_off && !cleared) {
    return;
  }
  const uint32_t renderer = Read32(base, kRendererPointer);
  if (!LooksLikePointer(renderer)) {
    return;
  }
  const uint32_t mask = Read32(base, renderer + kPhaseEnableMask);
  if (want_off) {
    if (!cleared) {
      were_set = mask & kShadowPhases;
      cleared = true;
      REXLOG_INFO("perf options: shadow phases off (mask {:#x})", mask);
    }
    // Every frame: the game may turn them back on.
    if (mask & kShadowPhases) {
      Write32(base, renderer + kPhaseEnableMask, mask & ~kShadowPhases);
    }
  } else {
    Write32(base, renderer + kPhaseEnableMask, mask | were_set);
    cleared = false;
    REXLOG_INFO("perf options: shadow phases back on");
  }
}

// --- Off-screen passes drawn every few frames ----------------------------------------

// Render phase bits (launcher/dev/frame-structure-2026-10-16.md): the sun-shadow
// cascades are 0x20, 0x40, 0x80 and 0x100 (the 0x100 phase also redraws the foliage
// impostors), the environment map 0x4 and 0x8, the reflection view 0x2.
constexpr uint32_t kPhaseReflection = 0x2;
constexpr uint32_t kPhaseEnvironmentMap = 0x4 | 0x8;
constexpr uint32_t kPhaseFarShadows = 0x80 | 0x100;

// The phases to leave out of this frame. Called on the render thread at the start
// of the phase loop, once a frame, so every phase of one option is left out together,
// and the options' frames fall apart (a different offset each) to even out the load.
uint32_t PhasesToSkipThisFrame() {
  static uint32_t frame = 0;
  ++frame;
  struct Rule {
    int32_t interval;
    uint32_t phases;
    uint32_t offset;
  };
  const Rule rules[] = {
      {REXCVAR_GET(mcla_far_shadow_interval), kPhaseFarShadows, 0},
      {REXCVAR_GET(mcla_env_map_interval), kPhaseEnvironmentMap, 1},
      {REXCVAR_GET(mcla_reflection_interval), kPhaseReflection, 2},
  };
  uint32_t skip = 0;
  for (const Rule& rule : rules) {
    if (rule.interval > 1 &&
        frame % uint32_t(rule.interval) != rule.offset % uint32_t(rule.interval)) {
      skip |= rule.phases;
    }
  }
  return skip;
}

// --- Anti-aliasing ----------------------------------------------------------------

// The renderer's multisample mode, which the game sets when it creates the
// renderer (sub_822E4AB0, where mcla_msaa's hook picks none at start-up) and
// reads while it renders: 2 is the 2x MSAA it uses on an HD display, 1 none
// (what its own developer switches give). Written at the swap
// (MclaApplyMsaaAtSwap, from mcla_present_interval), on the presenting thread
// between two frames, so that no frame sees two modes. Turning it off while
// the scene is drawn as one tile waits until a few frames have been drawn in
// two tiles (src/render_perf.cpp, mcla_single_tile): going from a one-tile
// frame in one mode to a one-tile frame in the other left the vehicle pass in
// the old mode.
constexpr uint32_t kMsaaMode = 384;
constexpr uint32_t kMsaaNone = 1;
constexpr uint32_t kMsaaGame = 2;

// Whether FSR can be used: the GPU plugin's temporal_aa_available (unknown
// until the first frame has ended; no where its runtime wasn't built or isn't
// there, the device lacks what it needs or it failed; next_start where it can
// after a restart, the device having been made without its features: the PS5
// started without FSR chosen).
bool FsrAvailable() {
  const std::string available = rex::cvar::GetFlagByName("temporal_aa_available");
  return available == "yes" || available == "unknown";
}
// Whether it can run after a restart only.
bool FsrNextStart() {
  return rex::cvar::GetFlagByName("temporal_aa_available") == "next_start";
}
// Whether to offer it: also when it applies at the next start.
bool FsrOffered() {
  return FsrAvailable() || FsrNextStart();
}

// mcla_anti_aliasing as it applies, read under the option registry's lock
// (the pause menu sets it from another thread): FSR where it can't run is
// MSAA.
std::string AntiAliasing() {
  std::string anti_aliasing = rex::cvar::GetFlagByName("mcla_anti_aliasing");
  if (anti_aliasing == "fsr" && !FsrAvailable()) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      if (FsrOffered()) {
        REXLOG_INFO("perf options: FSR takes effect at the next start; MSAA until then");
      } else {
        REXLOG_WARN("perf options: FSR is unavailable here; MSAA is used instead");
      }
    }
#if REX_PLATFORM_PS5
    // The console's device got FSR's features for this choice (main_ps5.cpp).
    static bool fell_back = false;
    if (!fell_back && rex::cvar::GetFlagByName("temporal_aa_available") == "no") {
      fell_back = true;
      MclaFallBackFromFsr();
    }
#endif
    return "msaa";
  }
  return anti_aliasing;
}

void ApplyMsaa(uint8_t* base) {
  static int applied = -1;
  static uint32_t game_mode = kMsaaGame;
  static int held = 0;
  const std::string anti_aliasing = AntiAliasing();
  // Temporal anti-aliasing (the SDK's temporal_aa, rexglue-temporal-aa.patch):
  // its jitter starts with the next frame, once FSR is ready (it is made in
  // the background the first time, half a second or so).
  static std::string applied_temporal;
  const char* temporal = anti_aliasing == "fsr" ? "fsr" : "off";
  if (applied_temporal != temporal) {
    rex::cvar::SetFlagByName("temporal_aa", temporal);
    if (!applied_temporal.empty() || anti_aliasing == "fsr") {
      REXLOG_INFO("perf options: FSR {}", anti_aliasing == "fsr" ? "on" : "off");
    }
    applied_temporal = temporal;
  }
  // FSR upscaling (the SDK's temporal_aa_upscale, rexglue-fsr-upscaling.patch): the ratio
  // of mcla_upscaling while FSR is the anti-aliasing, 1 (native) otherwise. Set when it
  // changes, so that a manual temporal_aa_upscale (development pads) stays as it is.
  static std::string applied_upscale;
  const char* upscale = "1";
  if (anti_aliasing == "fsr") {
    const std::string mode = rex::cvar::GetFlagByName("mcla_upscaling");
    // 4/3 to the digits of a double, so that the render size is exactly 3/4 of the output's
    // (1920x1080 of 2560x1440), whatever the rounding of a shorter number.
    upscale = mode == "ultra_quality" ? "1.3333333333333333"
              : mode == "quality"     ? "1.5"
              : mode == "balanced"    ? "1.7"
              : mode == "performance" ? "2"
                                      : "1";
  }
  if (applied_upscale != upscale) {
    rex::cvar::SetFlagByName("temporal_aa_upscale", upscale);
    if (!applied_upscale.empty() || std::strcmp(upscale, "1") != 0) {
      REXLOG_INFO("perf options: FSR upscaling {}", std::strcmp(upscale, "1") != 0 ? upscale : "off");
    }
    applied_upscale = upscale;
  }
  const int want = anti_aliasing == "msaa" ? 1 : 0;
  if (want == applied) {
    held = 0;
    return;
  }
  const uint32_t renderer = Read32(base, kRendererPointer);
  if (!LooksLikePointer(renderer)) {
    return;
  }
  const uint32_t mode = Read32(base, renderer + kMsaaMode);
  // 60 swaps at most, as a loading screen may draw no tiled pass at all.
  if (!want && mode != kMsaaNone && held < 60 && !MclaSingleTileReadyForMsaaChange()) {
    ++held;
    return;
  }
  held = 0;
  if (!want && mode != kMsaaNone) {
    game_mode = mode;
    Write32(base, renderer + kMsaaMode, kMsaaNone);
  } else if (want && mode == kMsaaNone) {
    Write32(base, renderer + kMsaaMode, game_mode);
  }
  if (applied >= 0) {
    REXLOG_INFO("perf options: anti-aliasing {}", want ? "on" : "off");
  }
  applied = want;
}

// --- Ambient density ------------------------------------------------------------

// The tuning fields at the start of an ambient zone.
constexpr uint32_t kZoneTrafficUnspawn = 0x10;  // 400.0 as shipped
constexpr uint32_t kZonePedestrians = 0x60;
constexpr uint32_t kZoneParkedCars = 0x98;

struct Zone {
  uint32_t address;
  float unspawn, pedestrians, parked;  // as the game set them
};
std::mutex g_zones_mutex;
std::vector<Zone> g_zones;
double g_applied_distance = 400.0, g_applied_pedestrians = 1.0, g_applied_parked = 1.0;

void WriteZone(uint8_t* base, const Zone& zone) {
  const double distance = REXCVAR_GET(mcla_traffic_distance);
  // The option is the shipped zone's 400 m scaled; zones keep their own ratio.
  WriteFloat(base, zone.address + kZoneTrafficUnspawn, float(zone.unspawn * (distance / 400.0)));
  WriteFloat(base, zone.address + kZonePedestrians,
             float(zone.pedestrians * REXCVAR_GET(mcla_pedestrians)));
  WriteFloat(base, zone.address + kZoneParkedCars,
             float(zone.parked * REXCVAR_GET(mcla_parked_cars)));
}

void ApplyAmbientIfChanged(uint8_t* base) {
  const double distance = REXCVAR_GET(mcla_traffic_distance);
  const double pedestrians = REXCVAR_GET(mcla_pedestrians);
  const double parked = REXCVAR_GET(mcla_parked_cars);
  if (distance == g_applied_distance && pedestrians == g_applied_pedestrians &&
      parked == g_applied_parked) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_zones_mutex);
  for (const Zone& zone : g_zones) {
    WriteZone(base, zone);
  }
  g_applied_distance = distance;
  g_applied_pedestrians = pedestrians;
  g_applied_parked = parked;
  REXLOG_INFO("perf options: ambient density applied to {} zones", g_zones.size());
}

}  // namespace

namespace {

uint64_t NowMicros() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void SetReduced(const std::string& what, bool reduced) {
  const bool all = what == "all";
  if (all || what == "shadows") {
    rex::cvar::SetFlagByName("mcla_shadows", reduced ? "false" : "true");
  }
  if (all || what == "foliage") {
    rex::cvar::SetFlagByName("mcla_foliage_shadows", reduced ? "false" : "true");
  }
  if (all || what == "lod") {
    rex::cvar::SetFlagByName("mcla_city_lod", reduced ? "0.5" : "1.0");
    rex::cvar::SetFlagByName("mcla_traffic_lod", reduced ? "0.5" : "1.0");
  }
  if (all || what == "density") {
    rex::cvar::SetFlagByName("mcla_traffic_distance", reduced ? "250" : "400");
    rex::cvar::SetFlagByName("mcla_pedestrians", reduced ? "0.5" : "1.0");
    rex::cvar::SetFlagByName("mcla_parked_cars", reduced ? "0.5" : "1.0");
  }
}

void TickAbTest() {
  static const std::string what = REXCVAR_GET(mcla_ab_test);
  if (what.empty()) {
    return;
  }
  struct State {
    uint64_t frames = 0, micros = 0, draws = 0;
  };
  static State states[2];
  static bool reduced = false;
  static uint64_t switched_at = 0, last_frame = 0, last_report = 0;
  const uint64_t now = NowMicros();
  if (!switched_at) {
    switched_at = last_frame = last_report = now;
    return;
  }
  // The first second after a switch is the change settling, not the state.
  if (now - switched_at > 1'000'000) {
    State& state = states[reduced ? 1 : 0];
    ++state.frames;
    state.micros += now - last_frame;
    state.draws += uint64_t(rex::perf::GetSnapshotCounter(rex::perf::CounterId::kDrawCalls));
  }
  last_frame = now;
  if (now - switched_at > 4'000'000) {
    reduced = !reduced;
    SetReduced(what, reduced);
    switched_at = now;
  }
  if (now - last_report > 30'000'000) {
    last_report = now;
    for (int i = 0; i < 2; ++i) {
      const State& state = states[i];
      if (state.frames) {
        REXLOG_INFO("ab test '{}' {}: {} frames, {:.2f} ms a frame, {:.0f} draws a frame", what,
                    i ? "reduced" : "as shipped", state.frames,
                    double(state.micros) / double(state.frames) / 1000.0,
                    double(state.draws) / double(state.frames));
      }
    }
  }
}

}  // namespace

// Once a frame, from the frame timer hook.
void MclaTickPerfOptions() {
  uint8_t* base = Base();
  if (!base) {
    return;
  }
  TickAbTest();
  ApplyShadowPhases(base);
  ApplyAmbientIfChanged(base);
}

// Before mcla_anti_aliasing, mcla_msaa = false turned anti-aliasing off: it
// still does while mcla_anti_aliasing isn't set (its default becomes off, which
// isn't saved as if the player had chosen it).
// For the pause menu, which offers FSR only where it can run.
bool MclaFsrNextStart() {
  return FsrNextStart();
}

bool MclaFsrAvailable() {
  return FsrOffered();
}

// Whether to offer FSR's upscaling levels: FSR can run now, and the GPU plugin has not
// said that the scene can't be drawn smaller here (rasterizer-ordered-view render
// targets, or it gave up after a resolve it couldn't handle).
bool MclaUpscalingAvailable() {
  return FsrAvailable() && rex::cvar::GetFlagByName("temporal_aa_upscale_available") != "no";
}

// The phases the render thread leaves out of the frame it is about to draw (0 for
// none); src/render_markers.cpp clears them in the renderer's mask for the frame.
uint32_t MclaRenderPhasesToSkip() {
  return PhasesToSkipThisFrame();
}

// mcla_preset: the options a preset stands for take its values as their defaults, so
// that one chosen by hand (the command line, the environment, mcla.toml) still wins,
// and none is saved as if the player had chosen it. Low is what was worth its look on the
// integrated Radeon (launcher/dev/gpu-weak-2026-10-27.md): anti-aliasing off (6.5% of the
// frame at native resolution, 29% at 2x), the far shadows every 2nd frame and the
// environment map every 4th (about 1 ms each at native), the full-screen blur (0.5 ms).
// The reflection view stays every frame: every 2nd saved about 0.3 ms, too little for a
// reflection that lags. Lowest is Low and the real-time shadows off, by far the largest
// saving left (launcher/dev/aniso-2026-10-27.md: 16 to 17% of the frame on every GPU
// measured; Low 12.3% and Lowest 26.9% on the integrated Radeon), at the price of every
// sun shadow.
void MclaApplyPreset() {
  const std::string preset = REXCVAR_GET(mcla_preset);
  if (preset != "low" && preset != "lowest") {
    return;
  }
  constexpr std::pair<const char*, const char*> kLow[] = {
      {"mcla_anti_aliasing", "off"},
      {"mcla_far_shadow_interval", "2"},
      {"mcla_env_map_interval", "4"},
      {"mcla_fullscreen_blur", "false"},
  };
  constexpr std::pair<const char*, const char*> kLowest[] = {
      {"mcla_shadows", "false"},
  };
  auto apply = [&preset](const auto& options) {
    for (const auto& [name, value] : options) {
      if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) {
        continue;
      }
      if (!rex::cvar::SetDefaultValue(name, value)) {
        REXLOG_WARN("preset {}: {} = {} not taken", preset, name, value);
      }
    }
  };
  apply(kLow);
  if (preset == "lowest") {
    apply(kLowest);
  }
  REXLOG_INFO("perf options: preset {}", preset);
}

void MclaMigrateAntiAliasing() {
  if (rex::cvar::GetFlagSource("mcla_anti_aliasing") == rex::cvar::Source::kDefault &&
      !REXCVAR_GET(mcla_msaa)) {
    rex::cvar::SetDefaultValue("mcla_anti_aliasing", "off");
    REXLOG_INFO("perf options: anti-aliasing off (mcla_msaa = false)");
  }
}

// The readback rules of the custom-vinyl texture: the game bakes a car's vinyls into a 512x512 8_8_8_8
// render target (guest address 0x050A0000, 1 MB) with its mip chain (256x256 at 0x051A0000 down to 32x4 at
// 0x051F7000), resolved in the UI phase 113 times while a race of 8 cars loads (launcher/dev/vinyls-2026-10-27.md).
// Nothing else in the city, the menus or a race has that size. The class rule finds the base level if the
// address moves; the address rule adds the chain.
static const char* const kVinylReadbackRules = "len=1048576,pitch=512,height=512,fmt=6;addr=0x051A0000-0x051F7FFF";

// The rain's particle state: the game's rain (the object sub_823047D8 makes, drawn by sub_822EE700) keeps the
// position and the velocity of 16,384 drops in four 128x128 k_32_32_FLOAT render targets, updated by one draw
// of four colour targets in the first render phase and resolved to four 128 KB textures (guest 0x0561F000,
// 0x0563F000, 0x0565F000, 0x0567F000 in the frame logs), the same memory that the rain's vertex shader fetches
// as four vertex buffers (launcher/dev/rain-unscaled-copy-2026-10-28.md). No other resolve has that size, pitch,
// height and format.
static const char* const kRainUnscaledCopyRule = "len=131072,pitch=128,height=128,fmt=37";

// resolution_scale, or draw_resolution_scale_x / _y, above 1 (what the SDK's texture cache reads, in the same
// order of precedence; any of them above 1 is enough here).
static bool DrawResolutionScaled() {
  for (const char* name : {"resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y"}) {
    if (std::strtol(rex::cvar::GetFlagByName(name).c_str(), nullptr, 10) > 1) {
      return true;
    }
  }
  return false;
}

void MclaApplyGraphicsFixDefaults(bool vulkan) {
  auto apply = [](const char* name, const char* value) {
    const bool user_set = rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault;
    if (!rex::cvar::SetDefaultValue(name, value)) {
      REXLOG_WARN("graphics fixes: {} = {} not taken (this SDK has no such option)", name, value);
    } else if (user_set) {
      REXLOG_INFO("graphics fixes: {} = {} set by the user", name, rex::cvar::GetFlagByName(name));
    } else {
      REXLOG_INFO("graphics fixes: {} = {}", name, value);
    }
  };
  if (REXCVAR_GET(mcla_vinyl_readback)) {
    apply("readback_resolve_rules", kVinylReadbackRules);
  }
  if (REXCVAR_GET(mcla_rain_unscaled_copy) && DrawResolutionScaled()) {
    apply("resolve_unscaled_copy_rules", kRainUnscaledCopyRule);
  }
  if (vulkan && REXCVAR_GET(mcla_vulkan_depth_convert)) {
    apply("depth_float24_convert_in_pixel_shader", "true");
  }
}

// At every swap, between two frames: the anti-aliasing option's mode.
void MclaApplyMsaaAtSwap() {
  if (uint8_t* base = Base()) {
    ApplyMsaa(base);
  }
}

void mcla_dev_option_registered(PPCRegister& r3) {
  uint8_t* base = Base();
  const uint32_t node = r3.u32;
  if (!base || !LooksLikePointer(node)) {
    return;
  }
  const uint32_t name = Read32(base, node);
  if (!LooksLikePointer(name)) {
    return;
  }
  for (const DevSwitch& dev_switch : kDevSwitches) {
    if (std::strcmp(reinterpret_cast<const char*>(base + name), dev_switch.name) != 0) {
      continue;
    }
    if (dev_switch.wanted()) {
      if (const uint32_t value = DevSwitchValue()) {
        Write32(base, node + 4, value);
        REXLOG_INFO("perf options: developer switch '{}' set", dev_switch.name);
      }
    }
    return;
  }
}

bool mcla_foliage_shadows(PPCRegister& r11) {
  if (REXCVAR_GET(mcla_foliage_shadows)) {
    return false;
  }
  r11.u64 = 0;
  return true;
}

bool mcla_msaa(PPCRegister& r11) {
  if (AntiAliasing() == "msaa") {
    return false;
  }
  r11.u64 = 1;
  return true;
}

bool mcla_impostor_search_guard(PPCRegister& r3) {
  const uint8_t* base = Base();
  if (!base) {
    return false;
  }
  const uint32_t manager = r3.u32;
  if (!manager) {
    return true;
  }
  const uint32_t array = Read32(base, manager + 16);
  if (!array) {
    return true;
  }
  const uint32_t count = (uint32_t(base[array + 12]) << 8) | base[array + 13];
  const uint32_t entries = Read32(base, array + 8);
  if (!count || !entries) {
    return true;
  }
  // The search wants an entry with both of these set; let it run only if
  // there is one.
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = entries + i * 224;
    if (Read32(base, entry + 168) && Read32(base, entry + 196)) {
      return false;
    }
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("perf options: no impostor to refresh in {} slots; search skipped", count);
  }
  return true;
}

void mcla_city_lod(PPCRegister& f13) {
  f13.f64 *= REXCVAR_GET(mcla_city_lod);
}

void mcla_traffic_lod(PPCRegister& f0) {
  f0.f64 *= REXCVAR_GET(mcla_traffic_lod);
}

void mcla_ambient_zone(PPCRegister& r3) {
  uint8_t* base = Base();
  const uint32_t address = r3.u32;
  if (!base || !LooksLikePointer(address)) {
    return;
  }
  Zone zone{address, ReadFloat(base, address + kZoneTrafficUnspawn),
            ReadFloat(base, address + kZonePedestrians), ReadFloat(base, address + kZoneParkedCars)};
  std::lock_guard<std::mutex> lock(g_zones_mutex);
  bool known = false;
  for (Zone& existing : g_zones) {
    if (existing.address == address) {
      existing = zone;
      known = true;
    }
  }
  if (!known) {
    g_zones.push_back(zone);
  }
  WriteZone(base, zone);
}

bool mcla_ui_lights_guard(PPCRegister& r3) {
  const uint8_t* base = Base();
  const uint32_t node = r3.u32;
  if (base && LooksLikePointer(node) && Read32(base, node + 8) == 5) {
    return false;
  }
  static std::atomic<int> reported{0};
  if (reported.fetch_add(1) < 4) {
    REXLOG_WARN("guard: UI movie without a lights node ({:#010x}); lighting pass skipped", node);
  }
  return true;
}

bool mcla_brain_reset_guard(PPCRegister& r3) {
  const uint8_t* base = Base();
  const uint32_t context = r3.u32;
  if (!base || !LooksLikePointer(context)) {
    return false;
  }
  const uint32_t arguments = Read32(base, context + 8);
  if (LooksLikePointer(arguments) && LooksLikePointer(Read32(base, arguments))) {
    return false;
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_WARN("guard: racing AI reset with no brain; skipped");
  }
  return true;
}

bool mcla_map_cursor_guard(PPCRegister& r31, PPCRegister& r30, PPCRegister& r26) {
  if (r31.u32 != 0) {
    return false;
  }
  uint8_t* base = Base();
  if (!base) {
    return false;
  }
  if (r26.u32 != r30.u32) {
    std::memmove(base + r26.u32, base + r30.u32, 16);
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_WARN("guard: map cursor step found no neighbouring cell; position taken unclamped");
  }
  return true;
}

// sub_821D5510 is the game's data cache flush: a loop of dcbf/dcbst over a
// range, one 128-byte line at a time, called about 5,000 times a second. The
// host needs none of it. A fence stands in for the one thing other threads
// (the audio decoder and mixer) could rely on it for: that what was written is
// visible to them afterwards. r3 is left as it came in, as the original does.
extern "C" REX_FUNC(sub_821D5510) {
  (void)ctx;
  (void)base;
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

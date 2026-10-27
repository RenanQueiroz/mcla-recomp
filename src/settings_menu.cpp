// In-game settings menu.
//
// Adds submenus to the Settings tab of the game's own pause menu, with rows
// bound to this project's options. The rows are objects of the game's own
// option classes (checkbox and value rows), so they look and behave like the
// rest of the menu.
//
// Ported from LARecomp's src/mc_engine/pause_menu.cpp with its author's
// permission; the reverse engineering of the menu system is theirs. See
// config/settings_menu.toml for what each hook address is. Left out: their
// carbon-fibre, cutscene, language and native-renderer tabs.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#if !REX_PLATFORM_PS5
#include <rex/input/input_system.h>
#endif
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/ui/low_latency.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(mcla_motion_blur, true, "MCLA", "Motion blur");
REXCVAR_DEFINE_BOOL(mcla_depth_of_field, true, "MCLA", "Depth of field blur");
REXCVAR_DEFINE_BOOL(mcla_settings_menu, true, "MCLA",
                    "Add the settings submenus to the pause menu");

// Set by a host that keeps the settings file somewhere other than next to
// the executable (PS5).
const char* g_mcla_settings_path = nullptr;

#if REX_PLATFORM_WIN32
bool MclaRelaunchGame();  // src/mcla_app.cpp
#endif
#if !REX_PLATFORM_PS5
void MclaWaitForSaves();  // src/save_wait.cpp
#endif
bool MclaSingleTileAvailable();  // src/render_perf.cpp
int32_t MclaHalfDisplayCap(float refresh_hz);  // src/frame_timing.cpp
bool MclaFsrAvailable();  // src/perf_options.cpp
bool MclaFsrNextStart();  // src/perf_options.cpp

namespace {

// --- Guest addresses ---------------------------------------------------------

constexpr uint32_t kStringTableGlobal = 0x8286D7FC;
constexpr uint32_t kMenuSystemGlobal = 0x8286D804;
constexpr uint32_t kAllocStateFn = 0x8268D578;
constexpr uint32_t kUIObjectCtorFn = 0x8263AD60;
constexpr uint32_t kUIMenuVtable = 0x82020654;
constexpr uint32_t kGuestMallocFn = 0x82130528;
constexpr uint32_t kStackPushFn = 0x8268F6A8;
constexpr uint32_t kStackPopFn = 0x8268DFF0;
constexpr uint32_t kIsButtonClickedFn = 0x82661508;
constexpr uint32_t kFindMovieFn = 0x821F9FB8;
constexpr uint32_t kSetFlashIntFn = 0x825EE0E0;
constexpr uint32_t kGetFlashObjFn = 0x825ED480;
constexpr uint32_t kSetFlashStrFn = 0x827227B8;
constexpr uint32_t kTabStripRenderFn = 0x82630E40;  // (strip): its tabs, then the selected one
constexpr uint32_t kWidgetMovieContextSlot = 468;   // vtable offset: the widget's movie context

constexpr uint32_t kOptionRowCtorFn = 0x826349F0;  // (obj, nameKey, values, wrap)
constexpr uint32_t kLabelRowCtorFn = 0x82633D40;   // (obj, nameKey, childCap)
constexpr uint32_t kSetRowLabelFn = 0x8263B860;    // (obj, nameKey)
constexpr uint32_t kStrListVtable = 0x82091D04;    // {vtable, char** keys, count, stride}
constexpr uint32_t kToggleRowVtable = 0x8209788C;

constexpr uint32_t kStr_PAUSEMOVIE = 0x8201D028;
constexpr uint32_t kStr_menu = 0x82030A34;
constexpr uint32_t kStr_tabs_count = 0x8208D4DC;
constexpr uint32_t kStr_name = 0x8200CC24;

// Row object layout.
constexpr uint32_t kRowObjectSize = 240;
constexpr uint32_t kRowChecked = 208;    // u8, checkbox rows
constexpr uint32_t kRowHasBox = 209;     // u8
constexpr uint32_t kRowEnabled = 212;
constexpr uint32_t kRowProperty1 = 216;
constexpr uint32_t kRowSelect = 208;     // value rows: current value slot

// List view layout.
constexpr uint32_t kListRows = 176;         // row pointer array
constexpr uint32_t kListCountCap = 180;     // count:u16 then capacity:u16
constexpr uint32_t kListSelect = 184;
constexpr uint32_t kListTableSource = 192;
constexpr uint32_t kListStateSource = 196;
constexpr uint32_t kListScroll = 256;

// --- Guest memory helpers ----------------------------------------------------

uint8_t* Membase() {
  auto* runtime = rex::Runtime::instance();
  return runtime ? runtime->virtual_membase() : nullptr;
}

uint32_t Read32(uint32_t ea) {
  const uint8_t* p = Membase() + ea;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t Read16(uint32_t ea) {
  const uint8_t* p = Membase() + ea;
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void Write32(uint32_t ea, uint32_t value) {
  uint8_t* p = Membase() + ea;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void Write8(uint32_t ea, uint8_t value) {
  Membase()[ea] = value;
}

template <typename... Args>
uint32_t CallGuest(uint32_t function_address, Args... args) {
  auto* runtime = rex::Runtime::instance();
  PPCFunc* function = runtime ? runtime->function_dispatcher()->GetFunction(function_address)
                              : nullptr;
  if (!function) {
    REXLOG_WARN("settings menu: guest function 0x{:08X} is not available", function_address);
    return 0;
  }
  return rex::ppc::GuestToHostFunction<uint32_t>(function, static_cast<uint32_t>(args)...);
}

uint32_t AllocGuestString(const char* text) {
  const size_t size = std::strlen(text) + 1;
  uint32_t buffer = CallGuest(kGuestMallocFn, uint32_t(size));
  if (buffer) {
    std::memcpy(Membase() + buffer, text, size);
  }
  return buffer;
}

// --- The game's string table -------------------------------------------------

// The game's string hash (sub_821C9790).
uint32_t HashString(const char* text) {
  uint32_t hash = 0;
  const bool quoted = (*text == '"');
  if (quoted) {
    ++text;
  }
  while (*text) {
    char c = *text;
    if (quoted && c == '"') {
      break;
    }
    ++text;
    uint8_t ch = static_cast<uint8_t>(c);
    if (ch >= 'A' && ch <= 'Z') {
      ch += 32;
    } else if (ch == '\\') {
      ch = '/';
    }
    uint32_t v = ch + hash;
    hash = ((1025u * v) >> 6) ^ (1025u * v);
  }
  return 32769u * (((9u * hash) >> 11) ^ (9u * hash));
}

uint32_t HashMapLookup(uint32_t map, uint32_t hash) {
  uint32_t buckets = Read32(map);
  uint16_t bucket_count = Read16(map + 4);
  if (!bucket_count || !buckets) {
    return 0;
  }
  for (uint32_t node = Read32(buckets + 4 * (hash % bucket_count)); node; node = Read32(node + 8)) {
    if (Read32(node) == hash) {
      return Read32(node + 4);
    }
  }
  return 0;
}

bool HashMapInsert(uint32_t map, uint32_t hash, uint32_t value) {
  uint32_t buckets = Read32(map);
  uint16_t bucket_count = Read16(map + 4);
  if (!bucket_count || !buckets) {
    return false;
  }
  uint32_t node = CallGuest(kGuestMallocFn, 12u);
  if (!node) {
    return false;
  }
  const uint32_t slot = buckets + 4 * (hash % bucket_count);
  Write32(node + 0, hash);
  Write32(node + 4, value);
  Write32(node + 8, Read32(slot));
  Write32(slot, node);
  return true;
}

// Sets the display text for a string-table key, creating the entry if needed.
// Text buffers are 132 bytes.
void SetStringTableText(const char* key, const char* text) {
  uint32_t table = Read32(kStringTableGlobal);
  if (!table) {
    return;
  }
  size_t length = std::strlen(text);
  if (length > 131) {
    length = 131;
  }
  const uint32_t hash = HashString(key);
  uint32_t buffer = HashMapLookup(table + 16, hash);
  if (!buffer) {
    buffer = CallGuest(kGuestMallocFn, 132u);
    if (!buffer || !HashMapInsert(table + 16, hash, buffer)) {
      return;
    }
  }
  std::memcpy(Membase() + buffer, text, length);
  Membase()[buffer + length] = 0;
}

// --- The game's menu state tree ----------------------------------------------

void DetachMenuItem(uint32_t child) {
  uint32_t parent = Read32(child + 32);
  if (!parent) {
    return;
  }
  uint32_t previous = Read32(child + 40);
  uint32_t next = Read32(child + 36);
  if (previous) {
    Write32(previous + 36, next);
  } else {
    Write32(parent + 44, next);
  }
  if (next) {
    Write32(next + 40, previous);
  }
  Write32(child + 32, 0);
  Write32(child + 36, 0);
  Write32(child + 40, 0);
}

// Reimplements sub_8268CC80.
void AppendMenuItem(uint32_t parent, uint32_t child) {
  if (!parent || !child || Read32(child + 32) != 0) {
    return;
  }
  uint32_t head = Read32(parent + 44);
  if (!head) {
    Write32(parent + 44, child);
    Write32(child + 40, 0);
  } else {
    uint32_t last = head;
    for (uint32_t next; (next = Read32(last + 36)) != 0; last = next) {
    }
    Write32(last + 36, child);
    Write32(child + 40, last);
  }
  Write32(child + 32, parent);
  Write32(child + 36, 0);
}

uint32_t MenuRootNode() {
  uint32_t root = Read32(kMenuSystemGlobal);
  return root ? Read32(root + 52) : 0;
}

uint32_t MenuEngine() {
  uint32_t node = MenuRootNode();
  return node ? node + 4 : 0;
}

void SetStateName(uint32_t state, const char* name) {
  if (uint32_t buffer = AllocGuestString(name)) {
    Write32(state + 20, buffer);
  }
}

// A plain menu state, as the engine's own factory makes them.
uint32_t CreateMenuState(const char* name) {
  uint32_t engine = MenuEngine();
  if (!engine) {
    return 0;
  }
  uint32_t state = CallGuest(kAllocStateFn, engine);
  if (!state) {
    return 0;
  }
  const uint32_t index = Read32(engine + 80) - 1;
  HashMapInsert(engine + 88, HashString(name), index);
  SetStateName(state, name);
  Write32(state + 16, 0x42);
  for (uint32_t offset = 24; offset <= 44; offset += 4) {
    Write32(state + offset, 0);
  }
  return state;
}

// A "UIMenu" state: what the class factory (sub_82222080) builds for one.
uint32_t CreateUIMenuState(const char* name) {
  uint32_t engine = MenuEngine();
  if (!engine) {
    return 0;
  }
  uint32_t state = CallGuest(kGuestMallocFn, 56u);
  if (!state) {
    return 0;
  }
  CallGuest(kUIObjectCtorFn, state);
  Write32(state, kUIMenuVtable);
  uint32_t array = Read32(engine + 76);
  uint32_t count = Read32(engine + 80);
  if (!array) {
    return 0;
  }
  Write32(array + 4 * count, state);
  Write32(engine + 80, count + 1);
  HashMapInsert(engine + 88, HashString(name), count);
  SetStateName(state, name);
  return state;
}

bool MenuStackPush(uint32_t state) {
  uint32_t node = MenuRootNode();
  if (!node) {
    return false;
  }
  CallGuest(kStackPushFn, node + 776, node + 4, state, 0u);
  return true;
}

void MenuStackPop() {
  if (uint32_t node = MenuRootNode()) {
    CallGuest(kStackPopFn, node + 776, node + 4, 1u);
  }
}

// --- Options -----------------------------------------------------------------

std::string OptionGet(const char* name) {
  return rex::cvar::GetFlagByName(name);
}

void OptionSet(const char* name, const char* value) {
  REXLOG_INFO("settings menu: {} = {}", name, value);
  rex::cvar::SetFlagByName(name, value);
}

enum class ItemKind { kToggle, kStringChoice, kNumberChoice, kNumberRange };

struct ItemDef {
  const char* key;    // menu state name and string-table key; must be unique
  ItemKind kind;
  const char* option;
  const char* label;
  const char* const* values = nullptr;        // kStringChoice: option values
  const char* const* value_labels = nullptr;  // display text (kNumberChoice: optional)
  const double* numbers = nullptr;            // kNumberChoice
  int value_count = 0;
  const char* number_format = nullptr;  // kNumberRange: for the number's text (%s)
  const char* zero_label = nullptr;     // kNumberChoice, kNumberRange: shown instead of 0
  // kNumberRange: 0 (zero_label, first), then range_min to range_max in steps
  // of range_step. A value set by hand between the steps shows as it is, and
  // a step from it goes to the next one in that direction. The ends don't
  // wrap round, so that holding left or right stops there.
  double range_min = 0.0;
  double range_max = 0.0;
  double range_step = 0.0;
  // Another kNumberRange option this one must stay at or above
  // (range_partner_below) or at or below; 0 there counts as what AUTO stands
  // for. A step that would cross it stops at the last value that doesn't.
  const char* range_partner = nullptr;
  bool range_partner_below = false;
  // kNumberChoice, for the frame rate cap with frame generation on: each
  // number's text adds how many frames that shows (`60 (120 SHOWN)`), and
  // half_first makes numbers[0] (-1) HALF DISPLAY, the cap taken from the
  // display's refresh rate (MclaHalfDisplayCap).
  bool shown = false;
  bool half_first = false;
};

// Display text must stay within A-Z, 0-9, space, '+', '.', and parentheses.
// A value's text is also the name of the Flash element created for it, and
// characters such as '/' are path separators there (LARecomp found that the
// hard way: it crashes UI setup).
#if REX_PLATFORM_PS5
// The console's display is 60 Hz.
constexpr double kFpsTargets[] = {30.0, 60.0};
#else
// The launcher's list: 40 to 50 are for variable-refresh displays.
constexpr double kFpsTargets[] = {30.0, 40.0, 45.0, 50.0, 60.0, 120.0, 0.0};
#endif
constexpr int kFpsTargetCount = int(sizeof(kFpsTargets) / sizeof(kFpsTargets[0]));
#if !REX_PLATFORM_PS5
// With frame generation on, on a display of 100 Hz or more: HALF DISPLAY (-1) first.
constexpr double kFpsTargetsHalf[] = {-1.0, 30.0, 40.0, 45.0, 50.0, 60.0, 120.0, 0.0};
constexpr int kFpsTargetHalfCount = int(sizeof(kFpsTargetsHalf) / sizeof(kFpsTargetsHalf[0]));
#endif
// frame_generation (the SDK's): off, or AMD FSR 3.1; it changes during play, on
// both graphics APIs (src/perf_options.cpp has the same for anti-aliasing). Built for
// Linux and the console, but not yet run on either.
constexpr const char* kFrameGenerationValues[] = {"off", "fsr"};
#if REX_PLATFORM_WIN32
constexpr const char* kFrameGenerationLabels[] = {"OFF", "FSR 3.1"};
#else
constexpr const char* kFrameGenerationLabels[] = {"OFF", "FSR 3.1 (UNTESTED)"};
#endif
// low_latency (the SDK's): the GPU vendor's latency reducer, NVIDIA Reflex or AMD Anti-Lag 2,
// named in plain text by the row (RowLabel). Boost is Reflex's; frame generation turns it on and
// locks the row (ItemChangeable).
constexpr const char* kLowLatencyValues[] = {"off", "on", "boost"};
constexpr const char* kLowLatencyLabels[] = {"OFF", "ON", "ON + BOOST"};
// How often the HUD advances (src/frame_timing.cpp): 0 is every frame.
#if REX_PLATFORM_PS5
constexpr double kHudRates[] = {30.0, 60.0, 0.0};
#else
constexpr double kHudRates[] = {30.0, 60.0, 120.0, 0.0};
#endif
constexpr int kHudRateCount = int(sizeof(kHudRates) / sizeof(kHudRates[0]));
#if !REX_PLATFORM_PS5
// resolution_scale (the SDK's; kRequiresRestart): the render targets, caches
// and translated shaders are made for one scale when the GPU emulation starts.
constexpr const char* kScaleValues[] = {"1", "2", "3"};
constexpr const char* kScaleLabels[] = {"NATIVE (720P)", "2X (1440P)", "3X (2160P)"};
#endif
#if REX_PLATFORM_WIN32
// mcla_gpu_backend (src/mcla_app.cpp): chosen before the window opens.
constexpr const char* kBackendValues[] = {"any", "d3d12", "vulkan"};
constexpr const char* kBackendLabels[] = {"AUTO", "D3D12", "VULKAN"};
#endif
constexpr const char* kPromptValues[] = {"auto", "xbox", "playstation"};
constexpr const char* kPromptLabels[] = {"AUTO", "XBOX", "PLAYSTATION"};
constexpr const char* kIntroValues[] = {"normal", "fast", "skip"};
constexpr const char* kIntroLabels[] = {"NORMAL", "FAST", "SKIP (NO LOGO)"};
// mcla_display_gamma (src/display_gamma.cpp): the console's ramp for an HDTV,
// or sRGB for a PC monitor; applied at the next swap.
constexpr const char* kGammaValues[] = {"tv", "pc"};
constexpr const char* kGammaLabels[] = {"TV", "PC MONITOR"};
#if !REX_PLATFORM_PS5
// HDR output (the SDK's presenter, rexglue-hdr-*.patch): off, auto (when the
// display is in HDR mode) or on; peak (200 to 4,000 nits in steps of 50) and
// paper white (80 to 480 in steps of 10, the range of Windows' SDR content
// brightness), AUTO first for what the display reports; the highlights'
// reach above the game's white (mcla_hdr_highlights). All change during play,
// as they're stepped through.
constexpr const char* kHdrValues[] = {"off", "auto", "on"};
constexpr const char* kHdrLabels[] = {"OFF", "AUTO", "ON"};
constexpr double kHdrHighlights[] = {1.0, 1.5, 2.0, 3.0};
constexpr const char* kHdrHighlightLabels[] = {"NORMAL", "BRIGHT", "BRIGHTER", "BRIGHTEST"};
#endif

const ItemDef kDisplayItems[] = {
    {"PM_MxFps", ItemKind::kNumberChoice, "mcla_fps", "FPS TARGET", nullptr, nullptr, kFpsTargets,
     kFpsTargetCount, "%.0f", "UNCAPPED"},
    {"PM_MxFrameGeneration", ItemKind::kStringChoice, "frame_generation", "FRAME GENERATION",
     kFrameGenerationValues, kFrameGenerationLabels, nullptr, 2},
    {"PM_MxLowLatency", ItemKind::kStringChoice, "low_latency", "LOW LATENCY", kLowLatencyValues,
     kLowLatencyLabels, nullptr, 3},
    {"PM_MxHudRate", ItemKind::kNumberChoice, "mcla_ui_hud_rate", "HUD UPDATES", nullptr, nullptr,
     kHudRates, kHudRateCount, "%.0f PER SECOND", "EVERY FRAME"},
#if !REX_PLATFORM_PS5
    // Neither means anything on the console: there is one display mode, and
    // the PS5 host turns the emulated vsync off at every start.
    {"PM_MxVsync", ItemKind::kToggle, "vsync", "VSYNC"},
    {"PM_MxFullscreen", ItemKind::kToggle, "fullscreen", "FULLSCREEN"},
    {"PM_MxResolution", ItemKind::kStringChoice, "resolution_scale", "RESOLUTION", kScaleValues,
     kScaleLabels, nullptr, 3},
#endif
#if REX_PLATFORM_WIN32
    {"PM_MxBackend", ItemKind::kStringChoice, "mcla_gpu_backend", "GRAPHICS API", kBackendValues,
     kBackendLabels, nullptr, 3},
#endif
    {"PM_MxGamma", ItemKind::kStringChoice, "mcla_display_gamma", "DISPLAY GAMMA", kGammaValues,
     kGammaLabels, nullptr, 2},
#if !REX_PLATFORM_PS5
    {"PM_MxHdr", ItemKind::kStringChoice, "hdr", "HDR", kHdrValues, kHdrLabels, nullptr, 3},
    {.key = "PM_MxHdrPeak",
     .kind = ItemKind::kNumberRange,
     .option = "hdr_peak_nits",
     .label = "PEAK BRIGHTNESS",
     .number_format = "%s NITS",
     .zero_label = "AUTO",
     .range_min = 200.0,
     .range_max = 4000.0,
     .range_step = 50.0,
     .range_partner = "hdr_paper_white_nits",
     .range_partner_below = true},
    {.key = "PM_MxHdrPaper",
     .kind = ItemKind::kNumberRange,
     .option = "hdr_paper_white_nits",
     .label = "PAPER WHITE",
     .number_format = "%s NITS",
     .zero_label = "AUTO",
     .range_min = 80.0,
     .range_max = 480.0,
     .range_step = 10.0,
     .range_partner = "hdr_peak_nits"},
    {"PM_MxHdrHighlights", ItemKind::kNumberChoice, "mcla_hdr_highlights", "HIGHLIGHTS", nullptr,
     kHdrHighlightLabels, kHdrHighlights, 4},
#endif
    {"PM_MxMotionBlur", ItemKind::kToggle, "mcla_motion_blur", "MOTION BLUR"},
    {"PM_MxDof", ItemKind::kToggle, "mcla_depth_of_field", "DEPTH OF FIELD"},
};

// FPS TARGET (kDisplayItems[0]) with frame generation on: the same caps with
// what they show, and on a display of 100 Hz or more (HALF DISPLAY would be under
// 50 rendered under it) HALF DISPLAY first, which is the cap of its own
// (mcla_fps_frame_generation, src/frame_timing.cpp); mcla_fps stays the player's
// cap for when it is off.
const ItemDef kFpsItemShown = {.key = "PM_MxFps",
                               .kind = ItemKind::kNumberChoice,
                               .option = "mcla_fps",
                               .label = "FPS TARGET",
                               .numbers = kFpsTargets,
                               .value_count = kFpsTargetCount,
                               .number_format = "%.0f",
                               .zero_label = "UNCAPPED",
                               .shown = true};
#if !REX_PLATFORM_PS5
const ItemDef kFpsItemHalf = {.key = "PM_MxFps",
                              .kind = ItemKind::kNumberChoice,
                              .option = "mcla_fps_frame_generation",
                              .label = "FPS TARGET",
                              .numbers = kFpsTargetsHalf,
                              .value_count = kFpsTargetHalfCount,
                              .number_format = "%.0f",
                              .zero_label = "UNCAPPED",
                              .shown = true,
                              .half_first = true};
#endif

// Whether frame generation can't run (the presenter's status, not a setting): the
// device or the runtime isn't there, or it failed. The console's device gets
// FSR's features only when the game starts with FSR or frame generation chosen: until
// then it can be chosen, to take effect at the next start (NEXT START).
bool FrameGenerationNo() {
  return rex::ui::Presenter::GetFrameGenerationStatus().state ==
         rex::ui::Presenter::FrameGenerationState::kUnavailable;
}

bool FrameGenerationNextStart() {
  const auto status = rex::ui::Presenter::GetFrameGenerationStatus();
  return status.state == rex::ui::Presenter::FrameGenerationState::kUnavailable &&
         status.why == "NEXT START";
}

bool FrameGenerationUnavailable() {
  return FrameGenerationNo() && !FrameGenerationNextStart();
}

bool FrameGenerationOn() {
  return OptionGet("frame_generation") != "off" && !FrameGenerationNo();
}

// The low latency row: the SDK's status (the driver's feature, which GPU, working or not).
bool IsLowLatencyItem(const ItemDef& item) {
  return item.kind == ItemKind::kStringChoice && !std::strcmp(item.option, "low_latency");
}

// Where the reducer isn't there (no GPU support, no driver support) the row says why and stays
// OFF; frame generation turns it on and locks it.
bool LowLatencyUnavailable() {
  return rex::ui::LowLatency::GetStatus().state != rex::ui::LowLatency::State::kAvailable;
}

// Frame generation forces the reducer on (the SDK's policy), unless the config file's
// low_latency_frame_generation = "off" (troubleshooting) says it should not.
bool LowLatencyLocked() {
  return !LowLatencyUnavailable() && FrameGenerationOn() &&
         OptionGet("low_latency_frame_generation") == "auto";
}

enum class FpsMode { kNormal, kShown, kHalf };

FpsMode CurrentFpsMode() {
  if (!FrameGenerationOn()) {
    return FpsMode::kNormal;
  }
#if !REX_PLATFORM_PS5
  if (MclaHalfDisplayCap(rex::ui::Presenter::GetDisplayRefreshHz()) > 0) {
    return FpsMode::kHalf;
  }
#endif
  return FpsMode::kShown;
}

// The item as it is now: FPS TARGET's depends on frame generation.
const ItemDef& Resolve(const ItemDef& item) {
  if (&item != &kDisplayItems[0]) {
    return item;
  }
  switch (CurrentFpsMode()) {
#if !REX_PLATFORM_PS5
    case FpsMode::kHalf:
      return kFpsItemHalf;
#endif
    case FpsMode::kShown:
      return kFpsItemShown;
    default:
      return item;
  }
}

bool IsFrameGenerationItem(const ItemDef& item) {
  return item.kind == ItemKind::kStringChoice && !std::strcmp(item.option, "frame_generation");
}

// What the rows show depends on this: it changing makes the menu bring them in line.
std::string MenuStateKey() {
  const auto status = rex::ui::Presenter::GetFrameGenerationStatus();
  const auto low_latency = rex::ui::LowLatency::GetStatus();
  return std::to_string(int(CurrentFpsMode())) + "|" +
         std::to_string(int(rex::ui::Presenter::GetDisplayRefreshHz())) + "|" +
         OptionGet("frame_generation") + "|" + std::to_string(int(status.state)) + "|" +
         status.why + "|" + std::to_string(int(low_latency.state)) + "|" + low_latency.name + "|" +
         low_latency.why + "|" + (low_latency.boost_available ? "b" : "-") +
         OptionGet("low_latency") + OptionGet("low_latency_frame_generation");
}

// Detail the game can do without when the frame rate matters more; all as
// shipped by default (src/perf_options.cpp). Options the game reads once, when
// it starts, are kRequiresRestart, and their rows say "(NEXT START)" (RowLabel).
constexpr double kDetailScales[] = {0.5, 0.75, 1.0};
// mcla_anti_aliasing (src/perf_options.cpp): none, the game's 2x MSAA, or AMD
// FSR 3.1 at native resolution (the SDK's temporal_aa, rexglue-temporal-aa.patch),
// with its sharpening (temporal_aa_sharpness). All change during play; FSR
// starts once it is ready (half a second or so the first time).
constexpr const char* kAntiAliasingValues[] = {"off", "msaa", "fsr"};
#if REX_PLATFORM_WIN32
constexpr const char* kAntiAliasingLabels[] = {"OFF", "MSAA 2X", "FSR 3.1 NATIVE AA"};
#else
// Built for Linux and the console, but not yet run on either.
constexpr const char* kAntiAliasingLabels[] = {"OFF", "MSAA 2X", "FSR 3.1 (UNTESTED)"};
#endif
// While FSR can run only after a restart (the PS5 started without it chosen:
// its device features are enabled only then, ps5/game/main_ps5.cpp). A row's value is clipped
// at about 27 characters ("FSR 3.1 (NEXT START, UNTESTED)" lost its end and its arrow), so
// the plain label says UNTESTED and this one doesn't.
constexpr const char* kFsrNextStartLabel = "FSR 3.1 (NEXT START)";
constexpr int kAntiAliasingCount = int(sizeof(kAntiAliasingValues) / sizeof(kAntiAliasingValues[0]));
constexpr double kSharpness[] = {0.0, 0.2, 0.4, 0.6, 0.8, 1.0};
constexpr double kDensityScales[] = {0.0, 0.5, 1.0};
// How often an off-screen pass is drawn (src/perf_options.cpp): 0 is every frame.
constexpr double kPassIntervals[] = {0.0, 2.0, 4.0, 8.0};
constexpr double kTrafficDistances[] = {250.0, 325.0, 400.0};

const ItemDef kPerformanceItems[] = {
    {"PM_MxSingleTile", ItemKind::kToggle, "mcla_single_tile", "SINGLE TILE RENDERING"},
    {"PM_MxFenceYield", ItemKind::kToggle, "mcla_fence_yield", "GPU WAIT YIELD"},
    {"PM_MxShadows", ItemKind::kToggle, "mcla_shadows", "SHADOWS"},
    {"PM_MxFoliageShadows", ItemKind::kToggle, "mcla_foliage_shadows", "FOLIAGE SHADOWS"},
    {"PM_MxCityLod", ItemKind::kNumberChoice, "mcla_city_lod", "CITY DETAIL DISTANCE", nullptr,
     nullptr, kDetailScales, 3, "%.2f"},
    {"PM_MxTrafficLod", ItemKind::kNumberChoice, "mcla_traffic_lod", "TRAFFIC DETAIL DISTANCE",
     nullptr, nullptr, kDetailScales, 3, "%.2f"},
    {"PM_MxTrafficDist", ItemKind::kNumberChoice, "mcla_traffic_distance", "TRAFFIC DISTANCE",
     nullptr, nullptr, kTrafficDistances, 3, "%.0f"},
    {"PM_MxPeds", ItemKind::kNumberChoice, "mcla_pedestrians", "PEDESTRIANS", nullptr, nullptr,
     kDensityScales, 3, "%.1f", "NONE"},
    {"PM_MxParked", ItemKind::kNumberChoice, "mcla_parked_cars", "PARKED CARS", nullptr, nullptr,
     kDensityScales, 3, "%.1f", "NONE"},
    {"PM_MxRaceShadows", ItemKind::kToggle, "mcla_race_shadows", "RACE SHADOWS"},
    {"PM_MxFastCarShadows", ItemKind::kToggle, "mcla_fast_car_shadows", "SIMPLE CAR SHADOWS"},
    {"PM_MxImpostors", ItemKind::kToggle, "mcla_foliage_impostors", "DISTANT TREES"},
    {"PM_MxFsBlur", ItemKind::kToggle, "mcla_fullscreen_blur", "FULLSCREEN BLUR"},
    {"PM_MxFarShadows", ItemKind::kNumberChoice, "mcla_far_shadow_interval", "FAR SHADOW UPDATES",
     nullptr, nullptr, kPassIntervals, 4, "EVERY %.0f FRAMES", "EVERY FRAME"},
    {"PM_MxEnvMap", ItemKind::kNumberChoice, "mcla_env_map_interval", "ENVIRONMENT MAP UPDATES",
     nullptr, nullptr, kPassIntervals, 4, "EVERY %.0f FRAMES", "EVERY FRAME"},
    {"PM_MxReflection", ItemKind::kNumberChoice, "mcla_reflection_interval", "REFLECTION UPDATES",
     nullptr, nullptr, kPassIntervals, 4, "EVERY %.0f FRAMES", "EVERY FRAME"},
    {"PM_MxAntiAliasing", ItemKind::kStringChoice, "mcla_anti_aliasing", "ANTI ALIASING",
     kAntiAliasingValues, kAntiAliasingLabels, nullptr, kAntiAliasingCount},
    {"PM_MxFsrSharpness", ItemKind::kNumberChoice, "temporal_aa_sharpness", "FSR SHARPNESS",
     nullptr, nullptr, kSharpness, 6, "%.1f", "OFF"},
};

const ItemDef kControlItems[] = {
    {"PM_MxPrompts", ItemKind::kStringChoice, "mcla_button_prompts", "BUTTON PROMPTS",
     kPromptValues, kPromptLabels, nullptr, 3},
    {"PM_MxIntro", ItemKind::kStringChoice, "mcla_intro", "INTRO", kIntroValues, kIntroLabels,
     nullptr, 3},
#if !REX_PLATFORM_PS5
    {"PM_MxHideCursor", ItemKind::kToggle, "mcla_hide_cursor", "HIDE MOUSE POINTER"},
#endif
};

struct MenuDef {
  const char* button_key;  // button state name and its string-table key
  const char* label;       // button label and submenu title
  const char* menu_key;    // submenu state name
  const ItemDef* items;
  int item_count;
};

const MenuDef kMenus[] = {
    {"PM_MxTabDisplay", "DISPLAY", "MxDisplayMenu", kDisplayItems,
     int(sizeof(kDisplayItems) / sizeof(kDisplayItems[0]))},
    {"PM_MxTabPerf", "PERFORMANCE", "MxPerfMenu", kPerformanceItems,
     int(sizeof(kPerformanceItems) / sizeof(kPerformanceItems[0]))},
    {"PM_MxTabControls", "CONTROLS", "MxControlsMenu", kControlItems,
     int(sizeof(kControlItems) / sizeof(kControlItems[0]))},
};
constexpr int kMenuCount = int(sizeof(kMenus) / sizeof(kMenus[0]));
// (The list scrolls: its row count is the submenu's own, the arrays below only have to be as long.)
constexpr int kMaxRows = 24;

// An option the game reads only when it starts (kRequiresRestart).
bool RestartOnly(const char* option) {
  const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(option);
  return info && info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart;
}

// A restart-only row says "(NEXT START)", and "(RESTART NEEDED)" once its
// value differs from the one the game started with. SINGLE TILE RENDERING
// says "(UNAVAILABLE)" where the GPU emulates the console's EDRAM, too small
// for it (src/render_perf.cpp), and doesn't change then (ItemChangeable).
std::string RowLabel(const ItemDef& item, bool pending) {
  std::string label = item.label;
  if (IsLowLatencyItem(item)) {
    // The vendor's feature by its name (plain text): NVIDIA REFLEX, AMD ANTI LAG 2.
    const rex::ui::LowLatency::Status status = rex::ui::LowLatency::GetStatus();
    if (status.state == rex::ui::LowLatency::State::kAvailable) {
      label = status.name;
    }
  }
  if (RestartOnly(item.option)) {
    label += pending ? " (RESTART NEEDED)" : " (NEXT START)";
  }
  if (std::strcmp(item.option, "mcla_single_tile") == 0 && !MclaSingleTileAvailable()) {
    label += " (UNAVAILABLE)";
  }
#if !REX_PLATFORM_PS5
  // HDR while the display says it isn't in HDR mode (read when DISPLAY opens,
  // and about once a second while HDR isn't off): the picture stays SDR.
  if (std::strcmp(item.option, "hdr") == 0) {
    const rex::ui::Presenter::HdrDisplayInfo display = rex::ui::Presenter::GetLastHdrDisplayInfo();
    if (display.known && !display.hdr_enabled) {
      label += " (NOT AVAILABLE)";
    }
  }
#endif
  return label;
}

// SINGLE TILE RENDERING keeps its value where it can't take effect, so that a
// session with the console's EDRAM (edram_tiles = 2048, or a GPU that can't
// bind a larger one) doesn't save a value for the next one.
bool ItemChangeable(const ItemDef& item) {
  if (IsFrameGenerationItem(item)) {
    return !FrameGenerationUnavailable();  // OFF (and why) is all it offers then
  }
  if (IsLowLatencyItem(item)) {
    return !LowLatencyUnavailable() && !LowLatencyLocked();
  }
  return std::strcmp(item.option, "mcla_single_tile") != 0 || MclaSingleTileAvailable();
}

// Whether the row's label can change while the menus are open.
bool LabelChanges(const ItemDef& item) {
  return RestartOnly(item.option) || std::strcmp(item.option, "hdr") == 0 || IsLowLatencyItem(item);
}

// FSR (the last anti-aliasing choice) is offered only where it can run, now or
// after a restart (src/perf_options.cpp); where it can't, the game uses MSAA
// instead.
bool IsAntiAliasingWithoutFsr(const ItemDef& item) {
  return item.kind == ItemKind::kStringChoice && !std::strcmp(item.option, "mcla_anti_aliasing") &&
         !MclaFsrAvailable();
}

int ItemValueCount(const ItemDef& item) {
  if (IsAntiAliasingWithoutFsr(item)) {
    return item.value_count - 1;
  }
  if (IsFrameGenerationItem(item) && FrameGenerationUnavailable()) {
    return 1;
  }
  if (IsLowLatencyItem(item)) {
    if (LowLatencyUnavailable() || LowLatencyLocked()) {
      return 1;
    }
    return rex::ui::LowLatency::GetStatus().boost_available ? item.value_count
                                                            : item.value_count - 1;
  }
  return item.kind == ItemKind::kToggle ? 2 : item.value_count;
}

// --- kNumberRange --------------------------------------------------------------

double RangeValue(const ItemDef& item) {
  return std::strtod(OptionGet(item.option).c_str(), nullptr);
}

// The number with up to two decimals and no trailing zeros, within the
// characters a value's text may use.
std::string RangeText(const ItemDef& item, double value) {
  if (!(value > 0.0)) {
    return item.zero_label;
  }
  char number[32];
  std::snprintf(number, sizeof number, "%.2f", value);
  std::string text = number;
  text.erase(text.find_last_not_of('0') + 1);
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, item.number_format, text.c_str());
  return buffer;
}

// The next value in a direction: from 0 (first) to range_min, then by
// range_step; a value between steps goes to the nearest step that way. The
// ends stay where they are.
double RangeStep(const ItemDef& item, double value, int direction) {
  constexpr double kSlack = 1e-6;
  const double position = (value - item.range_min) / item.range_step;
  if (direction > 0) {
    if (!(value > 0.0) || value < item.range_min) {
      return item.range_min;
    }
    const double next = item.range_min + (std::floor(position + kSlack) + 1.0) * item.range_step;
    return next <= item.range_max + kSlack ? next : value;
  }
  if (!(value > item.range_min + kSlack)) {
    return 0.0;
  }
  if (value > item.range_max) {
    return item.range_max;
  }
  return item.range_min + (std::ceil(position - kSlack) - 1.0) * item.range_step;
}

// What 0 stands for in an HDR level option: what the display reports.
double AutoLevel(const char* option) {
#if !REX_PLATFORM_PS5
  const rex::ui::Presenter::HdrAutoLevels levels = rex::ui::Presenter::GetHdrAutoLevels();
  return std::strcmp(option, "hdr_peak_nits") == 0 ? levels.peak_nits : levels.paper_white_nits;
#else
  (void)option;
  return 0.0;
#endif
}

// RangeStep, kept on its side of range_partner (PEAK BRIGHTNESS never below
// PAPER WHITE): a step that would cross it stops at the last step that
// doesn't, or doesn't move.
double RangeStepLimited(const ItemDef& item, double value, int direction) {
  double next = RangeStep(item, value, direction);
  if (!(next > 0.0) || !item.range_partner) {
    return next;
  }
  double partner = std::strtod(OptionGet(item.range_partner).c_str(), nullptr);
  if (!(partner > 0.0)) {
    partner = AutoLevel(item.range_partner);
  }
  constexpr double kSlack = 1e-6;
  const double position = (partner - item.range_min) / item.range_step;
  if (item.range_partner_below && next < partner) {
    const double held = std::clamp(item.range_min + std::ceil(position - kSlack) * item.range_step,
                                   item.range_min, item.range_max);
    next = direction < 0 && held >= value ? value : held;
  } else if (!item.range_partner_below && next > partner) {
    const double held = std::clamp(item.range_min + std::floor(position + kSlack) * item.range_step,
                                   item.range_min, item.range_max);
    next = direction > 0 && held <= value ? value : held;
  }
  return next;
}

std::string ItemValueLabel(const ItemDef& item, int index) {
  switch (item.kind) {
    case ItemKind::kToggle:
      return index ? "ON" : "OFF";
    case ItemKind::kStringChoice:
      if (item.values == kAntiAliasingValues && !std::strcmp(item.values[index], "fsr") &&
          MclaFsrNextStart()) {
        return kFsrNextStartLabel;
      }
      if (IsFrameGenerationItem(item) && index == 1 && FrameGenerationNextStart()) {
        return kFsrNextStartLabel;
      }
      if (IsFrameGenerationItem(item) && index == 0 && FrameGenerationUnavailable()) {
        // OFF, and why it's all there is: the menu's letters (the SDK's text).
        const std::string why = rex::ui::Presenter::GetFrameGenerationStatus().why;
        return why.empty() ? "OFF (UNAVAILABLE)" : "OFF (" + why + ")";
      }
      if (IsLowLatencyItem(item)) {
        const rex::ui::LowLatency::Status status = rex::ui::LowLatency::GetStatus();
        if (status.state != rex::ui::LowLatency::State::kAvailable) {
          return status.why.empty() ? "OFF (NOT AVAILABLE HERE)" : "OFF (" + status.why + ")";
        }
        if (LowLatencyLocked()) {
          return "ON (FRAME GEN)";
        }
        std::string label = item.value_labels[index];
        return status.untested && index > 0 ? label + " (UNTESTED)" : label;
      }
      return item.value_labels[index];
    case ItemKind::kNumberChoice: {
      if (item.value_labels) {
        return item.value_labels[index];
      }
      char buffer[96];
      if (item.half_first && index == 0) {
        const int32_t cap = MclaHalfDisplayCap(rex::ui::Presenter::GetDisplayRefreshHz());
        std::snprintf(buffer, sizeof buffer, "HALF DISPLAY %d", int(cap));
        return buffer;
      }
      if (item.zero_label && item.numbers[index] == 0.0) {
        return item.zero_label;
      }
      if (item.shown) {
        std::snprintf(buffer, sizeof buffer, "%.0f (%.0f SHOWN)", item.numbers[index],
                      2.0 * item.numbers[index]);
        return buffer;
      }
      std::snprintf(buffer, sizeof buffer, item.number_format, item.numbers[index]);
      return buffer;
    }
    case ItemKind::kNumberRange:  // by value (RangeText)
      break;
  }
  return "";
}

int ItemCurrentIndex(const ItemDef& item) {
  const std::string value = OptionGet(item.option);
  switch (item.kind) {
    case ItemKind::kToggle:
      return value == "true" ? 1 : 0;
    case ItemKind::kStringChoice:
      if (IsAntiAliasingWithoutFsr(item) && value == "fsr") {
        return 1;  // MSAA, what the game uses then
      }
      if (IsFrameGenerationItem(item) && FrameGenerationUnavailable()) {
        return 0;
      }
      if (IsLowLatencyItem(item)) {
        const rex::ui::LowLatency::Status status = rex::ui::LowLatency::GetStatus();
        if (status.state != rex::ui::LowLatency::State::kAvailable) {
          return 0;
        }
        if (LowLatencyLocked()) {
          return 0;
        }
        if (value == "auto") {
          return status.on_by_default ? 1 : 0;
        }
        if (value == "boost" && !status.boost_available) {
          return 1;  // on, what the driver does then
        }
      }
      for (int i = 0; i < item.value_count; ++i) {
        if (value == item.values[i]) {
          return i;
        }
      }
      return 0;
    case ItemKind::kNumberChoice: {
      const double number = std::strtod(value.c_str(), nullptr);
      int best = 0;
      double best_distance = -1.0;
      for (int i = 0; i < item.value_count; ++i) {
        const double distance = number > item.numbers[i] ? number - item.numbers[i]
                                                         : item.numbers[i] - number;
        if (best_distance < 0.0 || distance < best_distance) {
          best_distance = distance;
          best = i;
        }
      }
      return best;
    }
    case ItemKind::kNumberRange:  // by value (RangeValue)
      break;
  }
  return 0;
}

std::atomic<bool> g_settings_dirty{false};

// The options the player changed through these menus: the only ones saved.
std::mutex g_changed_mutex;
std::vector<std::string> g_changed_options;

void NoteChanged(const char* option) {
  std::lock_guard lock(g_changed_mutex);
  for (const std::string& name : g_changed_options) {
    if (name == option) {
      return;
    }
  }
  g_changed_options.emplace_back(option);
}

void ItemSetIndex(const ItemDef& item, int index) {
  if (index < 0 || index >= ItemValueCount(item)) {
    return;
  }
  switch (item.kind) {
    case ItemKind::kToggle:
      OptionSet(item.option, index ? "true" : "false");
      break;
    case ItemKind::kStringChoice:
      OptionSet(item.option, item.values[index]);
      break;
    case ItemKind::kNumberChoice: {
      char buffer[32];
      std::snprintf(buffer, sizeof buffer, "%g", item.numbers[index]);
      OptionSet(item.option, buffer);
      break;
    }
    case ItemKind::kNumberRange:  // by value (ItemSetNumber)
      return;
  }
  NoteChanged(item.option);
  g_settings_dirty.store(true, std::memory_order_relaxed);
}

}  // namespace

// The PS5 started with FSR chosen, which gave its device FSR's features, and FSR
// can't run after all (it failed, or isn't in the build): the setting becomes
// MSAA, what the game uses and the menu shows, saved with the next settings save,
// so that the console doesn't keep starting with those features for nothing.
void MclaFallBackFromFsr() {
  OptionSet("mcla_anti_aliasing", "msaa");
  NoteChanged("mcla_anti_aliasing");
  g_settings_dirty.store(true, std::memory_order_relaxed);
  REXLOG_WARN("settings menu: FSR can't run here; anti-aliasing set to MSAA (saved with the next settings save)");
}

namespace {

void ItemSetNumber(const ItemDef& item, double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%g", value);
  OptionSet(item.option, buffer);
  NoteChanged(item.option);
  g_settings_dirty.store(true, std::memory_order_relaxed);
}

// --- Rows --------------------------------------------------------------------

// A value row shows three slots (previous, current, next) with the selection
// on the middle one. The game's row class writes one Flash element per value
// without checking how many the row clip has, and three is what its own
// numeric rows use, so the window never exceeds three however long the real
// list is. After every step the window is re-centred, which also makes a step
// readable: a selection that is no longer on slot 1 is the direction pressed.
constexpr int kRowWindow = 3;

struct MenuRows {
  uint32_t array = 0;
  uint32_t row[kMaxRows] = {};
  uint32_t name[kMaxRows] = {};      // the row's string-table key, a guest string
  int last_index[kMaxRows] = {};
  std::string label[kMaxRows];       // the text its label has now
  std::string start_value[kMaxRows];  // the option's value when the menus were built
  std::string state_key;              // MenuStateKey when the rows were last brought in line
  // kNumberRange: left or right held (Hold).
  int held_direction[kMaxRows] = {};
  uint64_t held_since_ms[kMaxRows] = {};
  uint64_t held_event_ms[kMaxRows] = {};
  uint64_t held_step_ms[kMaxRows] = {};
  int count = 0;
};

MenuRows g_rows[kMenuCount];
std::atomic<uint32_t> g_menu_button_key[kMenuCount] = {};
std::atomic<uint32_t> g_menu_state[kMenuCount] = {};
std::atomic<uint32_t> g_menu_title[kMenuCount] = {};
std::atomic<uint32_t> g_quit_button_key{0};
// RESTART GAME (Windows): pressed once it asks to be pressed again, which
// restarts the game if it comes within kRestartConfirmMs.
constexpr const char* kRestartKey = "PM_MxRestart";
constexpr const char* kRestartLabel = "RESTART GAME";
constexpr const char* kRestartConfirmLabel = "RESTART GAME (PRESS AGAIN)";
constexpr uint64_t kRestartConfirmMs = 5000;
std::atomic<uint32_t> g_restart_button_key{0};
std::atomic<uint64_t> g_restart_asked_ms{0};

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}
std::atomic<int> g_active_menu{-1};
std::atomic<bool> g_menus_created{false};

std::atomic<uint32_t> g_pause_tab{0};
// The tab strip the game rendered last, and the pause screen's while one of
// our submenus is open: entering one hides the strip and retitles the menu.
std::atomic<uint32_t> g_last_tab_strip{0};
std::atomic<uint32_t> g_pause_tab_strip{0};
std::atomic<uint32_t> g_settings_state{0};
std::atomic<uint32_t> g_last_list{0};
std::atomic<uint32_t> g_pause_list{0};

// The pause list's own row sources, saved while one of our submenus borrows it.
struct SavedList {
  uint32_t rows, count_cap, table, state, select, scroll;
};
SavedList g_saved_list{};

void WindowKey(const ItemDef& item, int slot, char* out, size_t size) {
  std::snprintf(out, size, "%s_w%d", item.key, slot);
}

void CentreRowWindow(MenuRows& rows, int i, const ItemDef& item, int index) {
  const int count = ItemValueCount(item);
  for (int slot = 0; slot < kRowWindow; ++slot) {
    char key[96];
    WindowKey(item, slot, key, sizeof key);
    const int value = ((index + slot - 1) % count + count) % count;
    SetStringTableText(key, ItemValueLabel(item, value).c_str());
  }
  rows.last_index[i] = index;
  Write32(rows.row[i] + kRowSelect, 1);
}

void CentreRangeWindow(MenuRows& rows, int i, const ItemDef& item, double value) {
  const double values[kRowWindow] = {RangeStep(item, value, -1), value,
                                     RangeStep(item, value, +1)};
  for (int slot = 0; slot < kRowWindow; ++slot) {
    char key[96];
    WindowKey(item, slot, key, sizeof key);
    SetStringTableText(key, RangeText(item, values[slot]).c_str());
  }
  Write32(rows.row[i] + kRowSelect, 1);
}

uint32_t BuildValueSet(const ItemDef& item) {
  uint32_t keys = CallGuest(kGuestMallocFn, uint32_t(4 * kRowWindow));
  if (!keys) {
    return 0;
  }
  for (int slot = 0; slot < kRowWindow; ++slot) {
    char key[96];
    WindowKey(item, slot, key, sizeof key);
    SetStringTableText(key, "");
    uint32_t key_string = AllocGuestString(key);
    if (!key_string) {
      return 0;
    }
    Write32(keys + uint32_t(4 * slot), key_string);
  }
  uint32_t set = CallGuest(kGuestMallocFn, 16u);
  if (!set) {
    return 0;
  }
  Write32(set + 0, kStrListVtable);
  Write32(set + 4, keys);
  Write32(set + 8, uint32_t(kRowWindow));
  Write32(set + 12, 1);
  return set;
}

bool BuildRows(int menu) {
  MenuRows& rows = g_rows[menu];
  if (rows.array) {
    return true;
  }
  const MenuDef& def = kMenus[menu];
  if (def.item_count > kMaxRows) {
    return false;
  }
  uint32_t array = CallGuest(kGuestMallocFn, uint32_t(4 * def.item_count));
  if (!array) {
    return false;
  }
  for (int i = 0; i < def.item_count; ++i) {
    const ItemDef& item = Resolve(def.items[i]);
    // The row constructor resolves its label through the string table once,
    // so the text has to exist first.
    rows.start_value[i] = OptionGet(item.option);
    rows.label[i] = RowLabel(item, false);
    SetStringTableText(item.key, rows.label[i].c_str());
    uint32_t name = AllocGuestString(item.key);
    uint32_t row = CallGuest(kGuestMallocFn, kRowObjectSize);
    if (!name || !row) {
      return false;
    }
    std::memset(Membase() + row, 0, kRowObjectSize);
    rows.name[i] = name;

    if (item.kind == ItemKind::kToggle) {
      // The checkbox class has no constructor of its own: the label row's
      // constructor runs and the checkbox vtable is written over it.
      CallGuest(kLabelRowCtorFn, row, name, 0u);
      Write32(row, kToggleRowVtable);
      Write8(row + kRowChecked, uint8_t(ItemCurrentIndex(item)));
      Write8(row + kRowHasBox, 1);
      Write32(row + kRowEnabled, 1);
      Write32(row + kRowProperty1, 0xFFFFFFFFu);
      rows.row[i] = row;
      rows.last_index[i] = ItemCurrentIndex(item);
    } else {
      uint32_t set = BuildValueSet(item);
      if (!set) {
        return false;
      }
      CallGuest(kOptionRowCtorFn, row, name, set, 1u);
      rows.row[i] = row;
      if (item.kind == ItemKind::kNumberRange) {
        CentreRangeWindow(rows, i, item, RangeValue(item));
      } else {
        CentreRowWindow(rows, i, item, ItemCurrentIndex(item));
      }
    }
    Write32(array + uint32_t(4 * i), row);
  }
  rows.count = def.item_count;
  rows.array = array;
  rows.state_key = MenuStateKey();
  REXLOG_INFO("settings menu: '{}' built with {} rows", def.label, rows.count);
  return true;
}

// Points the list at our rows. The renderer only reaches the row array when
// the two higher-priority sources are null, so those are cleared.
void InstallRows(int menu, uint32_t list) {
  MenuRows& rows = g_rows[menu];
  if (!list || !rows.array || rows.count <= 0) {
    return;
  }
  const uint32_t count_cap = (uint32_t(rows.count) << 16) | uint32_t(rows.count);
  Write32(list + kListStateSource, 0);
  Write32(list + kListTableSource, 0);
  Write32(list + kListRows, rows.array);
  Write32(list + kListCountCap, count_cap);
}

// Marks a restart-only row whose value now differs from the one the game
// started with, and HDR's when the display isn't in HDR mode. The row
// constructor caches its label, so it is set again (kSetRowLabelFn) after the
// string-table text changes.
void UpdateRowLabels(int menu) {
  MenuRows& rows = g_rows[menu];
  const MenuDef& def = kMenus[menu];
  for (int i = 0; i < rows.count; ++i) {
    const ItemDef& item = Resolve(def.items[i]);
    if (!LabelChanges(item)) {
      continue;
    }
    std::string label = RowLabel(item, OptionGet(item.option) != rows.start_value[i]);
    if (label == rows.label[i] || !rows.row[i] || !rows.name[i]) {
      continue;
    }
    rows.label[i] = std::move(label);
    SetStringTableText(item.key, rows.label[i].c_str());
    CallGuest(kSetRowLabelFn, rows.row[i], rows.name[i]);
  }
}

// Brings every row in line with the current option values.
void SyncRowsFromOptions(int menu) {
  MenuRows& rows = g_rows[menu];
  const MenuDef& def = kMenus[menu];
  for (int i = 0; i < rows.count; ++i) {
    const ItemDef& item = Resolve(def.items[i]);
    const int index = ItemCurrentIndex(item);
    if (item.kind == ItemKind::kToggle) {
      rows.last_index[i] = index;
      Write8(rows.row[i] + kRowChecked, uint8_t(index));
    } else if (item.kind == ItemKind::kNumberRange) {
      CentreRangeWindow(rows, i, item, RangeValue(item));
    } else {
      CentreRowWindow(rows, i, item, index);
    }
  }
  UpdateRowLabels(menu);
}

// Holding left or right on a kNumberRange row. The game repeats a held
// direction after about 7 frames and then every 6 (40 ms apart at 150 FPS,
// 200 ms at 30), so a tap would step more than once at high frame rates and a
// hold would crawl at low ones. Here a press steps once; a hold steps again
// from kHoldDelayMs, at most every kHoldIntervalMs, then from kHoldFastMs on
// every repeat, and kHoldBigSteps times as far from kHoldBigMs. From
// kHoldFastMs a repeat steps once per 50 ms since the last one (up to 4), so
// the speed is about the same at any frame rate: PEAK BRIGHTNESS from 200 to
// 4,000 nits in under three seconds at 150 FPS and at 30. Whether an event
// repeats a held direction comes from the pad (MclaTickSettingsMenu), or,
// where the pad doesn't show the direction (another input), from the time
// since the last event.
constexpr uint64_t kHoldGapMs = 100;
constexpr uint64_t kHoldDelayMs = 400;
constexpr uint64_t kHoldIntervalMs = 100;
constexpr uint64_t kHoldFastMs = 1200;
constexpr uint64_t kHoldBigMs = 2200;
constexpr int kHoldBigSteps = 4;

// Left (0) and right (1), as user 0's pad reads them every frame while a
// submenu is open: whether each was let go since its last event, and whether
// it's down now.
std::atomic<bool> g_hold_released[2] = {true, true};
std::atomic<bool> g_hold_down[2] = {false, false};

int HoldSteps(MenuRows& rows, int i, int direction) {
  const uint64_t now = NowMs();
  const int side = direction < 0 ? 0 : 1;
  const bool released = g_hold_released[side].exchange(false, std::memory_order_relaxed);
  const bool repeat =
      rows.held_direction[i] == direction &&
      (g_hold_down[side].load(std::memory_order_relaxed) ? !released
                                                          : now - rows.held_event_ms[i] <= kHoldGapMs);
  const uint64_t gap = now - rows.held_event_ms[i];
  rows.held_event_ms[i] = now;
  if (!repeat) {
    rows.held_direction[i] = direction;
    rows.held_since_ms[i] = now;
    rows.held_step_ms[i] = now;
    return 1;
  }
  const uint64_t held = now - rows.held_since_ms[i];
  if (held < kHoldDelayMs || (held < kHoldFastMs && now - rows.held_step_ms[i] < kHoldIntervalMs)) {
    return 0;
  }
  rows.held_step_ms[i] = now;
  if (held < kHoldFastMs) {
    return 1;
  }
  // Repeats further apart than 50 ms (lower frame rates) step more each.
  const int steps = int(std::clamp<uint64_t>((gap + 25) / 50, 1, 4));
  return held < kHoldBigMs ? steps : steps * kHoldBigSteps;
}

// Left and right are handled inside the row, which then re-renders the list.
// The render is where the move is noticed and written to the option.
void PollRows(int menu) {
  MenuRows& rows = g_rows[menu];
  const MenuDef& def = kMenus[menu];
  for (int i = 0; i < rows.count; ++i) {
    const ItemDef& item = Resolve(def.items[i]);
    const int count = ItemValueCount(item);
    if (item.kind == ItemKind::kToggle) {
      const int checked = ItemCurrentIndex(item);
      if (checked != rows.last_index[i]) {
        rows.last_index[i] = checked;
        Write8(rows.row[i] + kRowChecked, uint8_t(checked));
      }
      continue;
    }
    const int slot = int(Read32(rows.row[i] + kRowSelect));
    if (slot != 1) {
      const int direction = slot < 1 ? -1 : +1;
      if (item.kind == ItemKind::kNumberRange) {
        const double value = RangeValue(item);
        double next = value;
        for (int step = HoldSteps(rows, i, direction); step > 0; --step) {
          next = RangeStepLimited(item, next, direction);
        }
        if (next != value) {
          ItemSetNumber(item, next);
        }
        CentreRangeWindow(rows, i, item, next);
        continue;
      }
      if (!ItemChangeable(item)) {
        CentreRowWindow(rows, i, item, ItemCurrentIndex(item));  // as it was
        continue;
      }
      const int index = ((rows.last_index[i] + direction) % count + count) % count;
      ItemSetIndex(item, index);
      CentreRowWindow(rows, i, item, index);
    }
  }
  // Every render: HDR's label follows the display, FPS TARGET and FRAME
  // GENERATION follow frame generation (turned on or off here or by another way,
  // or found to be unavailable) and the refresh rate.
  const std::string state_key = MenuStateKey();
  if (state_key != rows.state_key) {
    rows.state_key = state_key;
    SyncRowsFromOptions(menu);
  }
  UpdateRowLabels(menu);
}

// --- Pause list control ------------------------------------------------------

uint32_t PauseMovieContext() {
  uint32_t root = Read32(kMenuSystemGlobal);
  if (!root) {
    return 0;
  }
  uint32_t movie = CallGuest(kFindMovieFn, root, kStr_PAUSEMOVIE);
  return movie ? Read32(movie + 56) : 0;
}

void RefreshPauseList() {
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (!list) {
    return;
  }
  uint32_t vtable = Read32(list);
  uint32_t function = vtable ? Read32(vtable + 176) : 0;
  if (function) {
    CallGuest(function, list);
  }
}

void ShowSubmenu() {
  const int menu = g_active_menu.load(std::memory_order_relaxed);
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (menu < 0 || !list) {
    return;
  }
  InstallRows(menu, list);
  RefreshPauseList();

  uint32_t context = PauseMovieContext();
  if (!context) {
    return;
  }
  // Hide the tab strip so the movie presents a submenu, as the game's own
  // time-of-day menu does.
  CallGuest(kSetFlashIntFn, context, kStr_tabs_count, 0u);

  uint32_t title = g_menu_title[menu].load(std::memory_order_relaxed);
  if (!title) {
    title = AllocGuestString(kMenus[menu].label);
    g_menu_title[menu].store(title, std::memory_order_relaxed);
  }
  uint32_t menu_object = CallGuest(kGetFlashObjFn, context, HashString("menu"), kStr_menu);
  if (menu_object && title) {
    CallGuest(kSetFlashStrFn, menu_object, kStr_name, title);
  }
}

void RestorePauseList() {
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (!list) {
    return;
  }
  Write32(list + kListRows, g_saved_list.rows);
  Write32(list + kListCountCap, g_saved_list.count_cap);
  Write32(list + kListTableSource, g_saved_list.table);
  Write32(list + kListStateSource, g_saved_list.state);
  Write32(list + kListSelect, g_saved_list.select);
  Write32(list + kListScroll, g_saved_list.scroll);
  RefreshPauseList();
}

// --- Saving ------------------------------------------------------------------
//
// Only the options the player changed in these menus are written, merged into
// the file by rex::cvar::SaveConfigValues: each one's `name = value` line is
// replaced where it is or added before the first [table], every other line is
// kept, and the file is written beside the old one, flushed and renamed over
// it. (rex::cvar::SaveConfig, used before, rewrote the whole file with every
// option that differed from its default, wherever it came from: the launcher's
// command line, a host's own settings, the developer tools, and paths, which it
// wrote as TOML that no longer parsed.) A save that fails leaves the file as it
// was and is tried again at the next chance (leaving a submenu, closing).

std::mutex g_save_mutex;

void SaveSettingsIfChanged() {
  // From the game's thread (leaving a submenu, QUIT GAME, RESTART GAME) and
  // from the UI thread (the window closing).
  std::lock_guard save_lock(g_save_mutex);
  if (!g_settings_dirty.exchange(false, std::memory_order_relaxed)) {
    return;
  }
  std::vector<std::string> changed;
  {
    std::lock_guard lock(g_changed_mutex);
    changed = g_changed_options;
  }
  if (changed.empty()) {
    return;
  }
  // The file the runtime loads at start: <executable folder>/mcla.toml, or
  // the file a host without a writable executable folder names.
  const auto path = g_mcla_settings_path
                        ? std::filesystem::path(g_mcla_settings_path)
                        : rex::filesystem::GetExecutableFolder() / "mcla.toml";
  if (!rex::cvar::SaveConfigValues(path, changed)) {
    REXLOG_ERROR("settings menu: could not save to {}; will try again", path.string());
    g_settings_dirty.store(true, std::memory_order_relaxed);
    return;
  }
  REXLOG_INFO("settings menu: {} setting(s) saved to {}", changed.size(), path.string());
}

void CreateSubmenus() {
  if (g_menus_created.load(std::memory_order_relaxed)) {
    return;
  }
  uint32_t settings = g_settings_state.load(std::memory_order_relaxed);
  if (!settings) {
    return;
  }
  for (int menu = 0; menu < kMenuCount; ++menu) {
    const MenuDef& def = kMenus[menu];

    uint32_t button = CreateMenuState(def.button_key);
    if (!button) {
      return;
    }
    AppendMenuItem(settings, button);
    SetStringTableText(def.button_key, def.label);
    g_menu_button_key[menu].store(AllocGuestString(def.button_key), std::memory_order_relaxed);

    uint32_t submenu = CreateUIMenuState(def.menu_key);
    if (!submenu) {
      return;
    }
    // Give the submenu a parent pointer without listing it as a child: the
    // engine's "find my root" walk dereferences a null result for a state
    // with no parent.
    Write32(submenu + 32, settings);
    g_menu_state[menu].store(submenu, std::memory_order_relaxed);
    SetStringTableText(def.menu_key, def.label);

    // One child state per item keeps the tree the shape the engine expects,
    // although the list draws the row objects, not these.
    for (int i = 0; i < def.item_count; ++i) {
      if (uint32_t state = CreateMenuState(def.items[i].key)) {
        AppendMenuItem(submenu, state);
      }
    }
    BuildRows(menu);
  }

#if REX_PLATFORM_WIN32
  uint32_t restart = CreateMenuState(kRestartKey);
  uint32_t restart_key = AllocGuestString(kRestartKey);
  if (restart && restart_key) {
    AppendMenuItem(settings, restart);
    SetStringTableText(kRestartKey, kRestartLabel);
    g_restart_button_key.store(restart_key, std::memory_order_relaxed);
  }
#endif

  constexpr const char* kQuitKey = "PM_MxQuit";
  uint32_t quit = CreateMenuState(kQuitKey);
  uint32_t quit_key = AllocGuestString(kQuitKey);
  if (quit && quit_key) {
    AppendMenuItem(settings, quit);
    SetStringTableText(kQuitKey, "QUIT GAME");
    g_quit_button_key.store(quit_key, std::memory_order_relaxed);
  }
  g_menus_created.store(true, std::memory_order_relaxed);
  REXLOG_INFO("settings menu: {} submenus added to the pause menu", kMenuCount);
}

bool InImage(uint32_t address) {
  return address >= 0x82000000 && address < 0x82A00000;
}

// The tab strip the game rendered last, if it belongs to the pause screen.
// Checked to be one (its vtable holds the strip's render) before a virtual
// of it is called.
uint32_t PauseTabStrip() {
  const uint32_t strip = g_last_tab_strip.load(std::memory_order_relaxed);
  const uint32_t vtable = strip >= 0x10000 ? Read32(strip) : 0;
  if (!InImage(vtable)) {
    return 0;
  }
  bool is_strip = false;
  for (uint32_t slot = 0; slot < 160 && !is_strip; ++slot) {
    is_strip = Read32(vtable + 4 * slot) == kTabStripRenderFn;
  }
  const uint32_t context_fn = is_strip ? Read32(vtable + kWidgetMovieContextSlot) : 0;
  if (!InImage(context_fn)) {
    return 0;
  }
  const uint32_t context = CallGuest(context_fn, strip);
  return context && context == PauseMovieContext() ? strip : 0;
}

// Ends the game as the window's close button does: on the UI thread, after any
// career save in progress (MclaWaitForSaves), the game is started again if
// asked, and the window is closed, which ends the title and the process
// (ReXApp::OnClosing). WindowedAppContext::RequestDeferredQuit, used before,
// only left the UI loop: the app then waited for the game's main thread,
// which doesn't end, so the game played on behind a window that no longer
// answered. The PS5 host has no window listener of that kind, so it keeps the
// request it had.
void CloseGame(bool restart) {
  auto* runtime = rex::Runtime::instance();
#if REX_PLATFORM_PS5
  (void)restart;
  if (auto* context = runtime ? runtime->app_context() : nullptr) {
    context->RequestDeferredQuit();
  }
#else
  rex::ui::Window* window = runtime ? runtime->display_window() : nullptr;
  if (!window) {
    return;
  }
  window->app_context().CallInUIThread([window, restart] {
    MclaWaitForSaves();
#if REX_PLATFORM_WIN32
    if (restart && !MclaRelaunchGame()) {
      return;  // keep playing rather than quit without the new one
    }
#else
    (void)restart;
#endif
    window->RequestClose();
  });
#endif
}

bool ButtonClicked(uint32_t key) {
  return key && CallGuest(kIsButtonClickedFn, key, 1u) != 0;
}

// True when `list` is reachable from `node` through the widget child arrays.
// Most widgets keep children at +68; the tab strip keeps its tabs at +180.
bool NodeReachesList(uint32_t node, uint32_t list, int depth, int& budget) {
  if (!node || depth > 8 || --budget < 0) {
    return false;
  }
  for (uint32_t offset : {68u, 180u}) {
    uint32_t array = Read32(node + offset);
    uint16_t count = Read16(node + offset + 4);
    if (array < 0xB0000000 || !count || count > 64) {
      continue;
    }
    for (uint16_t i = 0; i < count; ++i) {
      uint32_t child = Read32(array + uint32_t(4 * i));
      if (child == list) {
        return true;
      }
      if (child >= 0xB0000000 && NodeReachesList(child, list, depth + 1, budget)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

// What was changed in these menus, saved now: the window is closing.
void MclaSaveMenuSettings() {
  SaveSettingsIfChanged();
}

// Every frame, on the thread that polls input (src/frame_timing.cpp): while a
// submenu is open, whether left and right are down (d-pad or left stick), for
// telling a held direction from new presses (HoldSteps).
void MclaTickSettingsMenu() {
#if !REX_PLATFORM_PS5
  if (g_active_menu.load(std::memory_order_relaxed) < 0) {
    return;
  }
  auto* input = static_cast<rex::input::InputSystem*>(
      rex::Runtime::instance() ? rex::Runtime::instance()->input_system() : nullptr);
  using rex::X_RESULT;  // in X_ERROR_SUCCESS
  rex::input::X_INPUT_STATE state = {};
  bool down[2] = {};
  if (input && input->GetState(0, &state) == X_ERROR_SUCCESS) {
    const uint16_t buttons = state.gamepad.buttons;
    const int16_t stick_x = state.gamepad.thumb_lx;
    down[0] = (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) || stick_x < -16384;
    down[1] = (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) || stick_x > 16384;
  }
  for (int side = 0; side < 2; ++side) {
    g_hold_down[side].store(down[side], std::memory_order_relaxed);
    if (!down[side]) {
      g_hold_released[side].store(true, std::memory_order_relaxed);
    }
  }
#endif
}

// --- Hooks -------------------------------------------------------------------

void mcla_menu_tab_strip(PPCRegister& r27) {
  g_last_tab_strip.store(r27.u32, std::memory_order_relaxed);
}

void mcla_menu_capture_continue(PPCRegister& r3) {
  if (r3.u32) {
    if (uint32_t pause_tab = Read32(r3.u32 + 32)) {
      g_pause_tab.store(pause_tab, std::memory_order_relaxed);
    }
  }
}

bool mcla_menu_enable_save(PPCRegister& r3, PPCRegister& r4) {
  if (!REXCVAR_GET(mcla_settings_menu)) {
    return false;
  }
  const uint32_t save_button = r3.u32;
  if (save_button && !g_settings_state.load(std::memory_order_relaxed)) {
    if (uint32_t parent = Read32(save_button + 32)) {
      g_settings_state.store(parent, std::memory_order_relaxed);
    }
  }
  uint32_t pause_tab = g_pause_tab.load(std::memory_order_relaxed);
  if (save_button && pause_tab) {
    DetachMenuItem(save_button);
    AppendMenuItem(pause_tab, save_button);
    SetStringTableText("Save (Dev Only)", "SAVE GAME");
  }
  CreateSubmenus();
  r4.u64 = 1;
  return true;
}

bool mcla_menu_click(PPCRegister& r3, PPCRegister& r31) {
  (void)r31;
  if (!g_menus_created.load(std::memory_order_relaxed)) {
    return false;
  }
  const int active = g_active_menu.load(std::memory_order_relaxed);
  if (active >= 0) {
    // Accept on a row steps its value forward. Every press inside a submenu is
    // consumed so the game never resolves it against the Settings tab's items.
    uint32_t list = g_pause_list.load(std::memory_order_relaxed);
    const uint32_t index = list ? Read32(list + kListSelect) : 0xFFFFFFFFu;
    const MenuDef& def = kMenus[active];
    if (index < uint32_t(def.item_count)) {
      const ItemDef& item = Resolve(def.items[index]);
      const int count = ItemValueCount(item);
      if (item.kind == ItemKind::kNumberRange) {
        const double value = RangeValue(item);
        const double next = RangeStepLimited(item, value, +1);
        if (next != value) {
          ItemSetNumber(item, next);
        }
      } else if (ItemChangeable(item)) {
        ItemSetIndex(item, (ItemCurrentIndex(item) + 1) % count);
      }
      SyncRowsFromOptions(active);
      ShowSubmenu();
    }
    r3.u64 = 1;
    return true;
  }

#if REX_PLATFORM_WIN32
  if (ButtonClicked(g_restart_button_key.load(std::memory_order_relaxed))) {
    // Progress since the last save is lost, as with QUIT GAME, so the first
    // press only asks for a second one.
    const uint64_t now = NowMs();
    const uint64_t asked = g_restart_asked_ms.load(std::memory_order_relaxed);
    if (!asked || now - asked > kRestartConfirmMs) {
      g_restart_asked_ms.store(now, std::memory_order_relaxed);
      SetStringTableText(kRestartKey, kRestartConfirmLabel);
      if (uint32_t list = g_last_list.load(std::memory_order_relaxed)) {
        const uint32_t vtable = Read32(list);
        if (const uint32_t render = vtable ? Read32(vtable + 176) : 0) {
          CallGuest(render, list);
        }
      }
      REXLOG_INFO("settings menu: restart asked for; press again to restart");
    } else {
      g_restart_asked_ms.store(0, std::memory_order_relaxed);
      SetStringTableText(kRestartKey, kRestartLabel);
      SaveSettingsIfChanged();
      REXLOG_INFO("settings menu: restarting");
      CloseGame(true);
    }
    r3.u64 = 1;
    return true;
  }
#endif

  if (ButtonClicked(g_quit_button_key.load(std::memory_order_relaxed))) {
    REXLOG_INFO("settings menu: quit requested");
    SaveSettingsIfChanged();
    CloseGame(false);
    r3.u64 = 1;
    return true;
  }

  for (int menu = 0; menu < kMenuCount; ++menu) {
    if (!ButtonClicked(g_menu_button_key[menu].load(std::memory_order_relaxed))) {
      continue;
    }
    uint32_t submenu = g_menu_state[menu].load(std::memory_order_relaxed);
    uint32_t list = g_last_list.load(std::memory_order_relaxed);
    if (!submenu || !list || !BuildRows(menu)) {
      return false;
    }
    g_pause_list.store(list, std::memory_order_relaxed);
    g_saved_list = {Read32(list + kListRows),        Read32(list + kListCountCap),
                    Read32(list + kListTableSource), Read32(list + kListStateSource),
                    Read32(list + kListSelect),      Read32(list + kListScroll)};
    Write32(list + kListSelect, 0);
    Write32(list + kListScroll, 0);

    g_active_menu.store(menu, std::memory_order_relaxed);
    g_pause_tab_strip.store(PauseTabStrip(), std::memory_order_relaxed);
#if !REX_PLATFORM_PS5
    // Read now, also with HDR off (it's only read while it isn't), for HDR's
    // "(NOT AVAILABLE)"; it usually takes well under a millisecond.
    for (int i = 0; i < kMenus[menu].item_count; ++i) {
      if (std::strcmp(kMenus[menu].items[i].option, "hdr") == 0 ||
          std::strcmp(kMenus[menu].items[i].option, "frame_generation") == 0) {
        rex::ui::Presenter::RefreshHdrDisplayInfo(250);
        break;
      }
    }
#endif
    SyncRowsFromOptions(menu);
    MenuStackPush(submenu);
    ShowSubmenu();
    REXLOG_INFO("settings menu: opened '{}'", kMenus[menu].label);
    r3.u64 = 1;
    return true;
  }
  return false;
}

bool mcla_menu_cancel(PPCRegister& r31) {
  (void)r31;
  if (g_active_menu.exchange(-1, std::memory_order_relaxed) < 0) {
    return false;
  }
  // Pop first, then hand the list back, in that order. Then the tab strip,
  // whose render also renders the selected tab again: the tabs and the
  // Settings title come back.
  MenuStackPop();
  RestorePauseList();
  if (uint32_t strip = g_pause_tab_strip.exchange(0, std::memory_order_relaxed)) {
    CallGuest(kTabStripRenderFn, strip);
  }
  SaveSettingsIfChanged();
  REXLOG_INFO("settings menu: closed");
  return true;
}

bool mcla_menu_populate_redirect(PPCRegister& r3) {
  const int menu = g_active_menu.load(std::memory_order_relaxed);
  uint32_t settings = g_settings_state.load(std::memory_order_relaxed);
  if (menu >= 0 && settings && r3.u32 == settings) {
    if (uint32_t submenu = g_menu_state[menu].load(std::memory_order_relaxed)) {
      r3.u64 = submenu;
    }
  }
  return false;
}

bool mcla_menu_list_populate(PPCRegister& r3) {
  const uint32_t list = r3.u32;
  if (!list) {
    return false;
  }
  if (const uint64_t asked = g_restart_asked_ms.load(std::memory_order_relaxed);
      asked && NowMs() - asked > kRestartConfirmMs) {
    g_restart_asked_ms.store(0, std::memory_order_relaxed);
    SetStringTableText(kRestartKey, kRestartLabel);
  }
  g_last_list.store(list, std::memory_order_relaxed);
  const int menu = g_active_menu.load(std::memory_order_relaxed);
  if (menu >= 0 && list == g_pause_list.load(std::memory_order_relaxed)) {
    PollRows(menu);
    InstallRows(menu, list);
  }
  return false;
}

bool mcla_menu_key_dispatch(PPCRegister& r31, PPCRegister& r28, PPCRegister& r27) {
  if (g_active_menu.load(std::memory_order_relaxed) < 0) {
    return false;
  }
  const uint32_t key = r28.u32 & 0xFF;
  if (key < '3' || key > '6') {  // left is '3' or '5', right is '4' or '6'
    return false;
  }
  uint32_t list = g_pause_list.load(std::memory_order_relaxed);
  if (!list) {
    return false;
  }
  int budget = 512;
  if (!NodeReachesList(r31.u32, list, 0, budget)) {
    return false;
  }
  uint32_t vtable = Read32(list);
  uint32_t function = vtable ? Read32(vtable + 32) : 0;
  if (!function) {
    return false;
  }
  CallGuest(function, list, key, r27.u32);
  return true;
}

bool mcla_menu_null_table_guard(PPCRegister& r30) {
  return r30.u32 == 0;
}

bool mcla_menu_list_vtable_guard(PPCRegister& r31) {
  const uint32_t list = r31.u32;
  return list && Read32(list) == 0;
}

bool mcla_motion_blur_gate(PPCRegister& r3) {
  if (REXCVAR_GET(mcla_motion_blur)) {
    return false;
  }
  r3.u64 = 0;
  return true;
}

void mcla_dof_composite(PPCRegister& r3) {
  if (REXCVAR_GET(mcla_depth_of_field) || !r3.u32) {
    return;
  }
  std::memset(Membase() + r3.u32 + 0xF0, 0, 16);
}

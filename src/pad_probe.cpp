// Title screen input: a development probe, and an option that keeps a button press visible to the
// game's scripts for a while (fork issue: Start has to be pressed many times on the title screen
// and the attract video; owner's report 2026-10-29).
//
// What the image does with a controller (read in the recompiled code, not changed by the fork):
// once per game update sub_82614D30 reads XInput into a 64-byte pad record (pad n at
// 0x82905388 + 64 n): the word at +12 is the previous update's button mask, the word at +8 the
// new one (LB 4, RB 8, Y 16, B 32, A 64, X 128, Back 256, left stick 512, right stick 1024,
// Start 2048, up 4096, right 8192, down 16384, left 32768). A press is "just pressed" for ONE
// update: (+8 xor +12) and +8. The game's scripts read it through the native sub_822C7FC8 (a
// button number in r4, a pad in r3); the menus' UI input object (sub_82223CD8) reads the masks
// itself and keeps byte counters of its own. On the console that is one update per 33 ms. At
// 120 FPS it is one per 8 ms, so a script that looks at the pad only every few updates (a wait
// of about one console frame) sees one press in a few.
//
// mcla_pad_probe (development): logs, with the time of the update, every button edge of the pad
// records, every "just pressed" a script reads (true results), the menu's key down events and
// whether the scene stack took them, and once a second how many updates and script reads there
// were. Every line starts with "TS ".
//
// mcla_title_edge_hold_ms (off by default, NOT measured as a fix yet): while the title screen,
// the start-up and the attract video are running (the lighting set-up that comes with the city
// has not run yet: the guard bit at 0x82836A28 is 0), a script that asks whether a button was
// just pressed is told yes for this many milliseconds after the press, not only in the update
// of the press. A script that asks in every update then sees the press in several updates in a
// row; the title and the attract video only move on.

#include "generated/default/mcla_init.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(mcla_pad_probe, false, "MCLA",
                    "Development: log the pad's button edges, the scripts' reads of them and the "
                    "menu's key events (lines starting with TS), for the title screen input check");
REXCVAR_DEFINE_INT32(mcla_title_edge_hold_ms, 0, "MCLA",
                     "On the title screen and the attract video, a script that asks whether a "
                     "button was just pressed is told yes for this many milliseconds after the "
                     "press (0 = only in the update of the press, as the game does)")
    .range(0, 500);

namespace {
constexpr uint32_t kPadBase = 0x82905388;
constexpr uint32_t kPadStride = 64;
constexpr uint32_t kPadCount = 4;
constexpr uint32_t kStartBit = 2048;
// Bit 0 of a function-local static's guard: 0 until the city's lighting set-up has run.
constexpr uint32_t kCityGuard = 0x82836A28;
// The dev pad's "game state" word and the "city pending" word (src/dev_pad.cpp), logged to see
// the title phase end.
constexpr uint32_t kGameState = 0x8288D000;
constexpr uint32_t kCityPending = 0x8286CF54;

uint64_t NowUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

uint32_t ReadGuestU32(uint32_t address) {
  if (address < 0x10000) {
    return 0;
  }
  const uint8_t* p = rex::Runtime::instance()->virtual_membase() + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool InTitlePhase() {
  return (ReadGuestU32(kCityGuard) & 1) == 0;
}

struct EdgeTime {
  uint64_t us = 0;
  uint64_t update = 0;
};

// Only the game's thread (the one that runs the updates and the scripts) touches these.
std::array<uint64_t, kPadCount> g_updates{};
std::array<std::array<EdgeTime, 32>, kPadCount> g_edge{};

std::atomic<uint64_t> g_next_report_us{0};
uint64_t g_report_updates = 0;
uint64_t g_native_calls = 0, g_native_true = 0, g_start_calls = 0, g_start_true = 0,
         g_delivered = 0, g_keys = 0;

void MaybeReport(uint64_t now) {
  uint64_t due = g_next_report_us.load(std::memory_order_relaxed);
  if (!due) {
    g_next_report_us.store(now + 1'000'000, std::memory_order_relaxed);
    return;
  }
  if (now < due) {
    return;
  }
  g_next_report_us.store(now + 1'000'000, std::memory_order_relaxed);
  REXLOG_INFO(
      "TS 1s t={} updates={} (pad0 total {}) native_calls={} true={} start_calls={} start_true={} "
      "held_deliveries={} keys={} title_phase={} (game state {}, city pending {})",
      now, g_updates[0] - g_report_updates, g_updates[0], g_native_calls, g_native_true,
      g_start_calls, g_start_true, g_delivered, g_keys, InTitlePhase(), ReadGuestU32(kGameState),
      ReadGuestU32(kCityPending));
  g_report_updates = g_updates[0];
  g_native_calls = g_native_true = g_start_calls = g_start_true = g_delivered = g_keys = 0;
}
}  // namespace

// 0x82615048 in sub_82614D30: the pad record (r31) has its new button mask (+8) and the previous
// one (+12); runs once per update for every pad that is connected.
void mcla_pad_probe_update(PPCRegister& r31) {
  const uint32_t pad = (r31.u32 - kPadBase) / kPadStride;
  if (r31.u32 < kPadBase || pad >= kPadCount) {
    return;
  }
  const bool probe = REXCVAR_GET(mcla_pad_probe);
  const bool hold = REXCVAR_GET(mcla_title_edge_hold_ms) > 0;
  if (!probe && !hold) {
    return;
  }
  const uint64_t now = NowUs();
  const uint64_t update = ++g_updates[pad];
  const uint32_t cur = ReadGuestU32(r31.u32 + 8);
  const uint32_t last = ReadGuestU32(r31.u32 + 12);
  const uint32_t pressed = cur & ~last;
  const uint32_t released = last & ~cur;
  for (uint32_t bit = 0; bit < 32 && pressed; bit++) {
    if (pressed & (1u << bit)) {
      g_edge[pad][bit] = {now, update};
    }
  }
  if (probe) {
    if (pressed || released) {
      REXLOG_INFO("TS pad{} edge t={} update={} down={:#06x} up={:#06x} cur={:#06x}", pad, now,
                  update, pressed, released, cur);
    }
    if (pad == 0) {
      MaybeReport(now);
    }
  }
}

// 0x822C8030 in sub_822C7FC8 (a script's "was this button just pressed"): r3 = the answer (the
// bit if it was, else 0), r11 = the pad record, r30 = the button number.
void mcla_pad_probe_native(PPCRegister& r3, PPCRegister& r11, PPCRegister& r30) {
  const bool probe = REXCVAR_GET(mcla_pad_probe);
  const int32_t hold_ms = REXCVAR_GET(mcla_title_edge_hold_ms);
  if (!probe && hold_ms <= 0) {
    return;
  }
  const uint32_t button = r30.u32;
  const uint32_t pad = (r11.u32 - kPadBase) / kPadStride;
  if (button >= 32 || r11.u32 < kPadBase || pad >= kPadCount) {
    return;
  }
  const uint64_t now = NowUs();
  bool answer = r3.u32 != 0;
  bool delivered = false;
  if (!answer && hold_ms > 0) {
    const EdgeTime& edge = g_edge[pad][button];
    if (edge.us && now - edge.us <= uint64_t(hold_ms) * 1000 && edge.update != g_updates[pad] &&
        InTitlePhase()) {
      // Pressed in an earlier update, still within the hold: the game's own answer was no.
      r3.u64 = 1ull << button;
      answer = true;
      delivered = true;
      g_delivered++;
    }
  }
  if (!probe) {
    return;
  }
  g_native_calls++;
  g_native_true += answer;
  if ((1u << button) == kStartBit) {
    g_start_calls++;
    g_start_true += answer;
  }
  if (answer) {
    REXLOG_INFO("TS pad{} script read: button {} just pressed{} t={} update={} (edge {} us before, update {})",
                pad, button, delivered ? " (held over)" : "", now, g_updates[pad],
                g_edge[pad][button].us ? int64_t(now - g_edge[pad][button].us) : -1,
                g_edge[pad][button].update);
  }
}

// 0x822240E4 in sub_82223CD8: after a menu key down was sent to the scene stack (sub_82209C38).
// r30 = whether something took it, r31 = the input (7 = Start), r4 = its key code (-1: none, and
// nothing was sent).
void mcla_pad_probe_key(PPCRegister& r30, PPCRegister& r31, PPCRegister& r4) {
  if (!REXCVAR_GET(mcla_pad_probe)) {
    return;
  }
  g_keys++;
  REXLOG_INFO("TS menu key down input={} key={} taken={} t={} update={}", r31.u32, r4.s32,
              r30.u32 & 0xFF, NowUs(), g_updates[0]);
}

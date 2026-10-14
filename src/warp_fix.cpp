// A camera-flight warp that never ends: the game stays on one shot of the
// city, the world keeps running (people walk, music plays) and no input does
// anything. Reported entering a Red Light race (issue #6); LARecomp found the
// same with the garage picked from the pause menu far from it. Ported from
// LARecomp's src/mc_engine/hooks/warper.cpp, with its author's permission.
//
// The game's warper (*(0x8286D8D4) + 0x51240) moves the player for an event
// or the garage behind a camera transition, and waits for it in state 19 or
// 20 (+288). State 20 finishes only when it predicts, one frame early, that
// the next step ends the transition:
//
//     time + dt + 0.0001 >= duration
//
// and then sets +276 so that the TRANSITION_COMPLETE event that follows
// finalizes the warp. The duration was measured stepping at a fixed 1/30 s,
// which is the console's frame. With real frame times (this project's frame
// timing hooks) the step that ends the transition can be longer than the dt
// the warper predicted with, or the transition can end a few ms short of the
// stored duration. The warper then never sees its last frame, the event
// arrives with +276 still clear, the handler skips the finalize, and the
// warper waits in state 20 for good.
//
// Fix: after the game's own warper update, a state 19 / 20 transition that
// was running and is not any more has ended. Do what the missed frame would
// have done (clear the GPS interlock, set +276 and state 29), then finalize as
// the handler would have: sub_8221B790 when TRANSITION_COMPLETE already came
// (+300 cleared), or the handler itself when it never did. A transition that
// never starts is given its duration plus 3 s before the same is done.

#include "generated/default/mcla_init.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mcla_warp_fix, true, "MCLA",
                    "Finish a camera-flight warp (event start, garage) whose flight ended "
                    "between two frames, which otherwise leaves the game on one shot forever");
REXCVAR_DEFINE_BOOL(mcla_warp_trace, false, "MCLA",
                    "Development: log every frame the warper waits on a transition");
REXCVAR_DEFINE_BOOL(mcla_warp_test_miss, false, "MCLA",
                    "Development: hide the running transition from the warper's own end check, "
                    "so every warp ends the way the stuck ones do. With mcla_warp_fix off this "
                    "reproduces the hang on demand");

// The game's own warper update (the generated body), its finalize and its
// TRANSITION_COMPLETE handler.
REX_EXTERN(__imp__sub_8221BE98);
REX_EXTERN(sub_8221B790);
REX_EXTERN(sub_8221B8C8);

namespace {

uint32_t ReadU32(const uint8_t* base, uint32_t address) {
  return (uint32_t(base[address]) << 24) | (uint32_t(base[address + 1]) << 16) |
         (uint32_t(base[address + 2]) << 8) | base[address + 3];
}

void WriteU32(uint8_t* base, uint32_t address, uint32_t value) {
  base[address] = uint8_t(value >> 24);
  base[address + 1] = uint8_t(value >> 16);
  base[address + 2] = uint8_t(value >> 8);
  base[address + 3] = uint8_t(value);
}

float ReadF32(const uint8_t* base, uint32_t address) {
  uint32_t bits = ReadU32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

constexpr uint32_t kCineScriptPtr = 0x8286D8F0;
constexpr uint32_t kCineGpsInterlock = 293;

constexpr uint32_t kWarperEventId = 256;
constexpr uint32_t kWarperDone = 276;
constexpr uint32_t kWarperState = 288;
constexpr uint32_t kWarperTransition = 296;
constexpr uint32_t kWarperPending = 300;
constexpr uint32_t kStateWaitTimed = 19;
constexpr uint32_t kStateWaitCinematic = 20;
constexpr uint32_t kStateIdle = 29;

constexpr uint32_t kTransitionFlags = 16;
constexpr uint32_t kTransitionName = 20;
constexpr uint32_t kTransitionDuration = 76;
constexpr uint32_t kTransitionTime = 96;
constexpr uint32_t kTransitionActive = 0x80;

constexpr float kNeverStartedGrace = 3.0f;

struct Watch {
  uint32_t transition = 0;
  bool seen_active = false;
  float waited = 0.0f;
  float last_time = 0.0f;
  float last_dt = 0.0f;
  uint32_t frames = 0;
};
Watch g_watch;  // only the game's UI thread updates the warper

bool IsWaitState(uint32_t state) {
  return state == kStateWaitTimed || state == kStateWaitCinematic;
}

std::string TransitionName(const uint8_t* base, uint32_t transition) {
  uint32_t name = ReadU32(base, transition + kTransitionName);
  if (!name) {
    return "<unnamed>";
  }
  const char* s = reinterpret_cast<const char*>(base + name);
  return std::string(s, strnlen(s, 64));
}

void FinishMissedWarp(PPCContext& ctx, uint8_t* base, uint32_t warper, uint32_t transition,
                      bool never_started) {
  float duration = ReadF32(base, transition + kTransitionDuration);
  uint32_t pending = ReadU32(base, warper + kWarperPending);
  if (never_started) {
    REXLOG_WARN("warp fix: '{}' never started; waited {:.2f} s (duration {:.3f} s), finishing it",
                TransitionName(base, transition), g_watch.waited, duration);
  } else {
    REXLOG_WARN("warp fix: '{}' ended between frames, last seen at {:.3f} s of {:.3f} s with dt "
                "{:.4f}; finishing the warp ({})",
                TransitionName(base, transition), g_watch.last_time, duration, g_watch.last_dt,
                pending ? "TRANSITION_COMPLETE not seen" : "after TRANSITION_COMPLETE");
  }

  // What states 19 and 20 do when their check fires.
  if (uint32_t cine = ReadU32(base, kCineScriptPtr)) {
    base[cine + kCineGpsInterlock] = 0;
  }
  base[warper + kWarperDone] = 1;
  WriteU32(base, warper + kWarperState, kStateIdle);

  // What TRANSITION_COMPLETE then does with +276 set.
  if (pending == transition) {
    ctx.r3.u64 = warper;
    ctx.r4.u64 = ReadU32(base, warper + kWarperEventId);
    ctx.r5.u64 = transition;
    sub_8221B8C8(ctx, base);
  } else {
    ctx.r3.u64 = warper;
    sub_8221B790(ctx, base);
  }
}

void WatchWarper(PPCContext& ctx, uint8_t* base, uint32_t warper, float dt) {
  if (!IsWaitState(ReadU32(base, warper + kWarperState))) {
    if (g_watch.transition && g_watch.frames) {
      REXLOG_INFO("warp fix: '{}' finished normally after {} frames ({:.2f} s)",
                  TransitionName(base, g_watch.transition), g_watch.frames, g_watch.waited);
    }
    g_watch = {};
    return;
  }
  uint32_t transition = ReadU32(base, warper + kWarperTransition);
  if (!transition) {
    return;
  }
  if (transition != g_watch.transition) {
    g_watch = {};
    g_watch.transition = transition;
    REXLOG_INFO("warp fix: waiting on '{}' ({:.3f} s)", TransitionName(base, transition),
                ReadF32(base, transition + kTransitionDuration));
  }

  bool active = (ReadU32(base, transition + kTransitionFlags) & kTransitionActive) != 0;
  float time = ReadF32(base, transition + kTransitionTime);
  float duration = ReadF32(base, transition + kTransitionDuration);
  ++g_watch.frames;
  g_watch.waited += dt;
  if (REXCVAR_GET(mcla_warp_trace)) {
    REXLOG_INFO("warp fix: frame {} dt {:.4f} active {} t {:.4f} / {:.4f}", g_watch.frames, dt,
                active ? 1 : 0, time, duration);
  }
  if (active) {
    g_watch.seen_active = true;
    g_watch.last_time = time;
    g_watch.last_dt = dt;
    return;
  }

  bool never_started =
      !g_watch.seen_active && g_watch.waited > std::max(duration, 0.0f) + kNeverStartedGrace;
  if (!g_watch.seen_active && !never_started) {
    return;
  }
  if (!REXCVAR_GET(mcla_warp_fix)) {
    if (g_watch.frames % 600 == 1) {
      REXLOG_WARN("warp fix: stuck on '{}' (mcla_warp_fix is off)",
                  TransitionName(base, transition));
    }
    return;
  }
  FinishMissedWarp(ctx, base, warper, transition, never_started);
  g_watch = {};
}

}  // namespace

// The warper's update, sub_8221BE98(warper, dt). The game's own state machine
// runs first; the watch looks at what it left behind.
extern "C" REX_FUNC(sub_8221BE98) {
  uint32_t warper = ctx.r3.u32;
  float dt = static_cast<float>(ctx.f1.f64);
  uint32_t hidden = 0;
  if (warper && REXCVAR_GET(mcla_warp_test_miss) &&
      IsWaitState(ReadU32(base, warper + kWarperState))) {
    uint32_t transition = ReadU32(base, warper + kWarperTransition);
    uint32_t flags = transition ? ReadU32(base, transition + kTransitionFlags) : 0;
    if (flags & kTransitionActive) {
      WriteU32(base, transition + kTransitionFlags, flags & ~kTransitionActive);
      hidden = transition;
    }
  }
  __imp__sub_8221BE98(ctx, base);
  if (hidden) {
    WriteU32(base, hidden + kTransitionFlags,
             ReadU32(base, hidden + kTransitionFlags) | kTransitionActive);
  }
  if (warper) {
    WatchWarper(ctx, base, warper, dt);
  }
}

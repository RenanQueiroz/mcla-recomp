// A car's shader variables, bound while the car is set up on another thread.
// Ported from LARecomp's src/mc_engine/hooks/vehicle.cpp (commit 74d4e8e), the same fix: the same game
// binary and guest addresses, the fork's own hook plumbing. The disassembly of every function named
// below is the same instruction for instruction in the fork's generated code and in LARecomp's.
//
// The object at vehicle + 1060 is a car's shader-variable cache (its constructor is sub_82377808, its
// per-update entry sub_823799D0, called by the car update sub_823567F8 on the thread that draws
// frames). It holds the car's shader group in two fields (+252 for the paint variables, +256 for the
// brake ones), the indices of the variables found in the group, and two "pending" bytes (+266, +267)
// that every reader tests before it touches the group.
//
//   sub_823793E8 (pending byte +266)  binds VinylTex, DmgTex, tintColors, useGradient,
//                                     materialHookups, ShadowImposterTex, shadowProjMtx
//   sub_823794E0 (pending byte +267)  binds BrakeRotorTempVal, brakeCaliperTintColor
//
// Each stores the group in its field, then calls sub_82193AF8(group, name, 0) once per variable,
// loading the group from the field again before every call, and clears its pending byte last.
// sub_82377B88 (the cache's reset, called only by sub_82353568, which sets a car up with a new model
// handle; its callers are sub_823320C8 and sub_82335AA8) sets both pending bytes and zeroes both group
// fields. When that lands on another thread between two of the calls above, the next call gets group
// 0 and sub_82193AF8's first read (lhz r27,12(r23), r23 = r3) reads guest 0xC: "Unhandled guest access
// violation: read of guest 0x0000000C". LARecomp logged one in 1,674 runs, on the render thread,
// about 6 s after the first free-roam frame.
//
// Three small guards, no option (a crash fix; nothing changes where the group is there):
//  - a lookup in no group finds nothing: the entry of sub_82193AF8 returns 0, the answer the
//    function already gives for a miss;
//  - a bind that ends with its group gone does not clear its pending byte (the `stb` that does is
//    skipped, to the function's epilogue), so the car binds again on the next update, which is
//    what the reset asked for. Every reader of the indices tests the pending byte first
//    (sub_82378368, sub_823784F8, sub_823780E8, sub_823783B8 and others).
// The first time each guard acts it logs one line (`[CarShaderBind]`), so a log shows the race
// happened.
//
// Is the race possible in the fork? The two threads are the game's own guest threads, each a host
// thread here as in LARecomp. The car update runs on the thread that draws frames: sub_82131008 ->
// sub_822C0980 (the frame function, render_markers.cpp) -> sub_822C5348 -> sub_822CB440 (reached
// through a function pointer) -> sub_823328E0 -> sub_823567F8, the chain of LARecomp's crash. The
// reset comes from the car's set-up (sub_822A7E08 and sub_82335AA8, which only function pointers
// call, so the code does not say on which thread). The GPU emulation runs no game code and is not
// one of the two threads. So the window is the same: a few microseconds, only open while a car is
// being set up (a world load, a traffic spawn, a race start). Nothing in the fork orders those
// two threads.
//
// CHECK: that sub_82353568 runs on a different guest thread from the render thread at that moment
// is inferred (the cache's only writer of those fields besides its constructor), not seen in a log;
// not seen in the fork at all yet (no log of it). Whether fork issue 77 (a crash when an event starts)
// is this race is unknown until its log shows `read of guest 0x0000000C` or a `[CarShaderBind]` line.
// The window between the test of the group and the `stb` that follows it (one instruction) is not
// closed, as in LARecomp: a reset that lands there leaves the car not pending with group 0. The
// lookups then miss (the first guard), but the readers of the indices (sub_82378368 and the others)
// would call sub_82192798 and its like with group 0 and a non-zero index; those were not read.
// That case was never seen.
//
// Hooks: config/car_shader_bind.toml.

#include "generated/default/mcla_init.h"

#include <atomic>
#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

namespace {

// The guard that acted, logged the first time it does.
std::atomic<bool> g_lookup_logged{false};
std::atomic<bool> g_brake_logged{false};
std::atomic<bool> g_paint_logged{false};

void NoteRace(std::atomic<bool>& logged, const char* where) {
  if (!logged.exchange(true)) {
    REXLOG_WARN(
        "[CarShaderBind] {}: the car's shader group was cleared while its variables were being bound "
        "(a car set up on another thread); it binds again on the next update",
        where);
  }
}

// True when the shader group in the cache's field at `offset` is null. One aligned 32-bit load of
// guest memory, so a store of 0 by another thread is seen whole or not at all.
bool GroupGone(const PPCRegister& cache, uint32_t offset) {
  auto* runtime = rex::Runtime::instance();
  const uint8_t* base = runtime ? runtime->virtual_membase() : nullptr;
  if (!base) {
    return false;
  }
  return *reinterpret_cast<const volatile uint32_t*>(base + cache.u32 + offset) == 0;
}

}  // namespace

// 0x82193AF8, the entry of sub_82193AF8: look the variable r4 up in the shader group r3 (r5 = 1 makes a
// miss fatal); the result is an index, 0 when nothing matched. A null group has no variables. True is a
// `blr` with r3 = 0, the answer the function gives for a miss.
bool mcla_shader_group_lookup(PPCRegister& r3) {
  if (r3.u32 != 0) {
    return false;
  }
  r3.u64 = 0;
  NoteRace(g_lookup_logged, "lookup");
  return true;
}

// 0x82379548 in sub_823794E0, before `stb r9,267(r31)`, which clears the brake bind's pending byte.
// r31 is the cache. True jumps to the epilogue at 0x8237954C.
bool mcla_car_shader_brake_done(PPCRegister& r31) {
  if (!GroupGone(r31, 256)) {
    return false;
  }
  NoteRace(g_brake_logged, "brake variables");
  return true;
}

// 0x823794C8 in sub_823793E8, before `stb r11,266(r31)`, which clears the paint bind's pending byte.
// r31 is the cache. True jumps to the epilogue at 0x823794CC.
bool mcla_car_shader_paint_done(PPCRegister& r31) {
  if (!GroupGone(r31, 252)) {
    return false;
  }
  NoteRace(g_paint_logged, "paint variables");
  return true;
}

// Render markers: tell the GPU emulation where the game's frame reaches the
// points that HDR output works from (the end of the 3D scene, the tone map,
// the HUD), in the command stream itself.
//
// The frame is drawn by sub_822C0980, whose render phase loop (sub_822E6408)
// walks the renderer's phase bits and calls sub_822E55D0 at the start of each
// phase it runs (r4 = the bit). Phase 0x800 starts the main view's 3D scene
// (everything before it draws into its own targets: reflections, shadows),
// 0x100000 its translucent effects, 0x04000000 its full-screen composites (the
// fog), 0x08000000 is post-processing, 0x10000000 the tone map (then the night
// light glows), 0x20000000 the HUD and menus
// (launcher/dev/frame-structure-2026-10-16.md). Temporal anti-aliasing
// (rexglue-temporal-aa.patch) jitters the scene's draws between the first and
// post-processing. At those calls a marker is
// written into Direct3D's command buffer: a PM4 type-3 NOP of 3 words, 'REXM',
// kind, value. The GPU skips NOPs, so the picture does not change; the SDK
// (rexglue-guest-markers.patch, CommandProcessor::OnGuestMarker) reads them
// where they fall among the game's draws. Kinds are the SDK's GuestMarkerKind.
//
// The device is at 0x82839254: [device+48] is its command buffer write
// pointer (the last word written; writers store from +4 on) and [device+56]
// the limit that its writers compare the pointer with, once, before writing up
// to 8 words; past it they make room with sub_82412710 (r3 = the device; it
// returns the new write pointer), and so do the markers. Markers are written
// only on the thread that draws frames and only between whole Direct3D calls
// (these function boundaries); a development version that also marked a
// render-state setter called inside other code corrupted the stream.
//
// Why making room here is safe: sub_82412710 is what every Direct3D packet
// writer calls first when the pointer is past the limit, before writing a word
// of its packet, so it runs whenever the device is between packets. The
// markers' call sites are such points: the frame function (sub_822C0980,
// called by the game loop sub_82131008), the phase loop (sub_822E6408, called
// by the frame function) and the phase start (sub_822E55D0, called by the
// phase loop and by sub_82360AE8) are renderer functions, never called from
// inside Direct3D, and none of them or their callers keeps the write pointer
// or limit (device +48, +56) across these calls (they only make Direct3D
// calls, which check and make room themselves). The game's next Direct3D call
// on this thread would make the same call in the same state.

#include "generated/default/mcla_init.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(mcla_render_markers, true, "MCLA",
                    "Mark the end of the 3D scene, the tone map and the HUD in the GPU command "
                    "stream, for HDR output (draws nothing)");
REXCVAR_DEFINE_BOOL(mcla_render_markers_all_phases, false, "MCLA",
                    "Development: also mark the start of every render phase");

namespace {

// The SDK's GuestMarkerKind (include/rex/graphics/command_processor.h).
enum MarkerKind : uint32_t {
  kFrameBegin = 1,
  kFrameEnd = 2,
  kSceneEnd = 3,
  kTonemapBegin = 4,
  kUIBegin = 5,
  kRenderEnd = 6,
  kSceneBegin = 7,
  kSceneTranslucent = 8,
  kSceneComposite = 9,
  kPhase = 0x100,
};

// The render phases that mean something for the markers.
constexpr uint32_t kPhaseScene = 0x800;
constexpr uint32_t kPhaseTranslucent = 0x100000;
constexpr uint32_t kPhaseComposite = 0x04000000;
constexpr uint32_t kPhasePost = 0x08000000;
constexpr uint32_t kPhaseTonemap = 0x10000000;
constexpr uint32_t kPhaseUI = 0x20000000;

constexpr uint32_t kD3DDevicePointer = 0x82839254;
constexpr uint32_t kDeviceWritePointer = 48;
constexpr uint32_t kDeviceWriteLimit = 56;

std::atomic<size_t> g_render_thread{0};
uint32_t g_frame = 0;
uint32_t g_markers_dropped = 0;
uint32_t g_markers_made_room = 0;

size_t ThisThread() {
  return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void StoreBe32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

void EmitMarker(PPCContext& ctx, uint8_t* base, uint32_t kind, uint32_t value) {
  if (!REXCVAR_GET(mcla_render_markers) || g_render_thread.load() != ThisThread()) {
    return;
  }
  const uint32_t device = LoadBe32(base, kD3DDevicePointer);
  uint32_t put = device >= 0x10000 ? LoadBe32(base, device + kDeviceWritePointer) : 0;
  if (put >= 0x10000 && put > LoadBe32(base, device + kDeviceWriteLimit)) {
    // Make room as the game's own writers do; the caller's registers are kept.
    PPCContext saved = ctx;
    ctx.r3.u64 = device;
    sub_82412710(ctx, base);
    put = ctx.r3.u32;
    ctx = saved;
    // The context's FPSCR is only a cache of the host's MXCSR: the call may
    // have changed the flush-to-zero mode (sub_82412ED0 turns it off), and
    // the copy put back above no longer says what MXCSR holds, so the
    // recompiled code would skip setting it. Put MXCSR back to the caller's
    // rounding and flush modes too.
    ctx.fpscr.restoreGuestBits(saved.fpscr.csr);
    // Logged at 1, 2, 4, 8... times. (The count is read once: ++x & (x - 1) in
    // one expression reads and modifies x unsequenced.)
    const uint32_t made_room = ++g_markers_made_room;
    if (!(made_room & (made_room - 1))) {
      REXLOG_INFO("Render markers: made room in the command buffer {} times", made_room);
    }
  }
  if (put < 0x10000) {
    const uint32_t dropped = ++g_markers_dropped;
    if (!(dropped & (dropped - 1))) {
      REXLOG_WARN("Render markers: {} skipped (no command buffer)", dropped);
    }
    return;
  }
  StoreBe32(base, put + 4, 0xC0021000);  // type 3, NOP, 3 words
  StoreBe32(base, put + 8, 0x5245584D);  // 'REXM'
  StoreBe32(base, put + 12, kind);
  StoreBe32(base, put + 16, value);
  StoreBe32(base, device + kDeviceWritePointer, put + 16);
}

}  // namespace

// The frame's rendering: the thread that calls it is the one that draws.
extern "C" REX_FUNC(sub_822C0980) {
  g_render_thread.store(ThisThread());
  EmitMarker(ctx, base, kFrameBegin, ++g_frame);
  __imp__sub_822C0980(ctx, base);
  EmitMarker(ctx, base, kFrameEnd, g_frame);
}

// The render phase loop; what is drawn after it in the frame (Bink movies) is
// over the finished picture.
extern "C" REX_FUNC(sub_822E6408) {
  __imp__sub_822E6408(ctx, base);
  EmitMarker(ctx, base, kRenderEnd, g_frame);
}

// The start of a render phase (r3 = the renderer, r4 = the phase bit).
extern "C" REX_FUNC(sub_822E55D0) {
  const uint32_t phase = ctx.r4.u32;
  if (phase == kPhaseScene) {
    EmitMarker(ctx, base, kSceneBegin, phase);
  } else if (phase == kPhaseTranslucent) {
    EmitMarker(ctx, base, kSceneTranslucent, phase);
  } else if (phase == kPhaseComposite) {
    EmitMarker(ctx, base, kSceneComposite, phase);
  } else if (phase == kPhasePost) {
    EmitMarker(ctx, base, kSceneEnd, phase);
  } else if (phase == kPhaseTonemap) {
    EmitMarker(ctx, base, kTonemapBegin, phase);
  } else if (phase == kPhaseUI) {
    EmitMarker(ctx, base, kUIBegin, phase);
  }
  if (REXCVAR_GET(mcla_render_markers_all_phases)) {
    EmitMarker(ctx, base, kPhase, phase);
  }
  __imp__sub_822E55D0(ctx, base);
}

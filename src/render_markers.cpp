// Render markers: tell the GPU emulation where the game's frame reaches the
// points that HDR output works from (the end of the 3D scene, the tone map,
// the HUD), in the command stream itself.
//
// The frame is drawn by sub_822C0980, whose render phase loop (sub_822E6408)
// walks the renderer's phase bits and calls sub_822E55D0 at the start of each
// phase it runs (r4 = the bit). Phase 0x08000000 is post-processing, 0x10000000
// the tone map (then the night light glows), 0x20000000 the HUD and menus
// (launcher/dev/frame-structure-2026-10-16.md). At those calls a marker is
// written into Direct3D's command buffer: a PM4 type-3 NOP of 3 words, 'REXM',
// kind, value. The GPU skips NOPs, so the picture does not change; the SDK
// (rexglue-guest-markers.patch, CommandProcessor::OnGuestMarker) reads them
// where they fall among the game's draws. Kinds are the SDK's GuestMarkerKind.
//
// The device is at 0x82839254: [device+48] is its command buffer write
// pointer (the last word written; writers store from +4 on) and [device+56]
// the limit that its writers compare the pointer with, once, before writing up
// to 8 words (sub_824199B0). Markers are written only on the thread that draws
// frames and only between whole Direct3D calls (these function boundaries); a
// development version that also marked a render-state setter called inside
// other code corrupted the stream.

#include "generated/default/mcla_init.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

#include <rex/cvar.h>

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
  kPhase = 0x100,
};

// The render phases that mean something for the markers.
constexpr uint32_t kPhasePost = 0x08000000;
constexpr uint32_t kPhaseTonemap = 0x10000000;
constexpr uint32_t kPhaseUI = 0x20000000;

constexpr uint32_t kD3DDevicePointer = 0x82839254;
constexpr uint32_t kDeviceWritePointer = 48;
constexpr uint32_t kDeviceWriteLimit = 56;

std::atomic<size_t> g_render_thread{0};
uint32_t g_frame = 0;

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

void EmitMarker(uint8_t* base, uint32_t kind, uint32_t value) {
  if (!REXCVAR_GET(mcla_render_markers) || g_render_thread.load() != ThisThread()) {
    return;
  }
  const uint32_t device = LoadBe32(base, kD3DDevicePointer);
  if (device < 0x10000) {
    return;
  }
  const uint32_t put = LoadBe32(base, device + kDeviceWritePointer);
  if (put < 0x10000 || put > LoadBe32(base, device + kDeviceWriteLimit)) {
    // Past the limit the game's own writers make room first; skip this one.
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
  EmitMarker(base, kFrameBegin, ++g_frame);
  __imp__sub_822C0980(ctx, base);
  EmitMarker(base, kFrameEnd, g_frame);
}

// The render phase loop; what is drawn after it in the frame (Bink movies) is
// over the finished picture.
extern "C" REX_FUNC(sub_822E6408) {
  __imp__sub_822E6408(ctx, base);
  EmitMarker(base, kRenderEnd, g_frame);
}

// The start of a render phase (r3 = the renderer, r4 = the phase bit).
extern "C" REX_FUNC(sub_822E55D0) {
  const uint32_t phase = ctx.r4.u32;
  if (phase == kPhasePost) {
    EmitMarker(base, kSceneEnd, phase);
  } else if (phase == kPhaseTonemap) {
    EmitMarker(base, kTonemapBegin, phase);
  } else if (phase == kPhaseUI) {
    EmitMarker(base, kUIBegin, phase);
  }
  if (REXCVAR_GET(mcla_render_markers_all_phases)) {
    EmitMarker(base, kPhase, phase);
  }
  __imp__sub_822E55D0(ctx, base);
}

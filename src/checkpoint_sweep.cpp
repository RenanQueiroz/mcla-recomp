// Checkpoints and line triggers that a script asks about: "did the car cross this since I last looked".
// Ported from LARecomp's src/mc_engine/hooks/checkpoint_sweep.cpp (commit 673228b; the entries per car from
// its commit a5a631d), the same fix: the same game binary and guest addresses, the fork's own hook plumbing.
//
// The game decides it with a swept segment. The race checkpoints are tested by sub_822653A8
// (checkpoint, end, start): a gate (a line checkpoint, +330) or a circle with a height band (+168
// radius, +180 / +184 heights) against the segment from `start` to `end`.
//  - CheckpointManager_HasHitCheckpoint (sub_82263930 -> sub_822633F0): end is the car's position
//    plus 2 m along its velocity, start is the racer's +400 (the position of the previous update).
//  - CheckpointManager_GetWhichHitCheckpoint (sub_822638F0 -> sub_82263600 -> sub_82263338, a loop
//    over the open checkpoints): end is the car's position, start is the same +400.
//  - mcTriggerLine::Check (sub_826881A8, the script's Trigger_CheckTrigger and, every update, the
//    trigger manager): the same segment, flattened, against the line.
// So the segment covers one update's travel. At 30 FPS the script runs in every update and sees every
// segment. Above 30 FPS a script thread looks only every 2nd to 4th update (see pad_probe.cpp), and
// the updates in between sweep a piece of road that no look ever tests: a gate or a small circle that
// the car crosses in one of those updates is never reported, however close the car was. HasHit's 2 m
// probe hides most of it (the car has to move more than 2 m between two looks); GetWhichHit's segment is
// one update long, so a gate is missed in about two looks out of three at 147 FPS.
//
// For every place in a script that asks (the running thread and its program counter, with the
// checkpoint or trigger, and the racer) it remembers where the segment ended at its previous look. A
// look that finds no hit and whose segment starts beyond that point tests the piece in between as
// well: the segment from the previous end to the new one, which contains the game's own. That is what
// the script would have been told at 30 FPS, where the previous look is the previous update. Nothing
// changes when the script looks in every update (its previous end is not behind the new start), and a
// car that jumped (a reset, a race start: more than kMaxGapM, or a previous look older than
// kWindowMs) starts afresh, so a teleport never sweeps through a gate.
//
// Measured in LARecomp (hangout 0x1F4, 6 passes): uncapped without the fix 2 of 6 entries, with it 6 of
// 6; 30 and 60 FPS 6 of 6 either way. Not run in the fork. No option turns it off: it changes nothing
// where the game's own test already answers, and a missed hangout is a game-breaking miss.
//
// CHECK: that the racer's +400 is the position of the previous update is read from how the three tests
// use it (every one sweeps from it to the current position) and from the trigger manager running the
// line test once per update; the code that writes it was not found. That HasHit's end is 2 m ahead
// along the car's velocity (sub_822A8148's vector, normalised, times 2.0 at 0x82040E94) is read from
// the code, not seen in a running game.
//
// One place can ask for several cars. The race script (the thread and opcode of LARecomp's race trace,
// 2026-11-12) asks the same checkpoint from the same opcode for the player and for every opponent in
// turn, so the checkpoint test cannot tell the cars apart by its arguments (it gets two vectors, not the
// racer). The previous ends are therefore kept per place AND per car, the car being found by where its
// segment starts: the entry of the place that ended nearest to the start (checkpoint_sweep_tracker.h).
// With one entry per place (v0.3.12) the second car found the first car's end 20 to 200 m away, no look
// was ever stretched in a race, and a gate (a line checkpoint, 46 to 161 m wide in the traced race) could
// still be missed by a car faster than the game's own probe covers: HasHit looks 2 m ahead, a script that
// looks every 33 ms (the race script's gap between looks, measured at 30 FPS and uncapped in LARecomp)
// leaves a stretch of speed x (33 ms - one update) - 2 m untested, i.e. above about 70 m/s at 240 FPS (77
// at 144, 120 at 60). The circles of a race (radius 22 to 49 m in the traced race) are crossed in more
// than half a second even at the fastest car's speed and are not at risk. The hangout line triggers need
// nothing new (their key already holds the racer), nor does a script that looks in every update, nor a
// teleport.
//
// The bookkeeping (checkpoint_sweep_tracker.h, standard library only) is checked without the game by
// launcher/dev/checkpoint-sweep/checkpoint_sweep_test.cpp: the rules of one look, several cars at one
// place, and a simulated race field against a gate at 30 to 360 FPS. The per-car entries are not run in
// the fork's game; a race gate rescued by them was not caught in a test.
//
// kWindowMs and kMaxGapM (checkpoint_sweep_tracker.h): a polling script's looks were 20 to 33 ms apart in
// the runs that measured it (pad_probe.cpp), 13 to 33 ms in LARecomp, so 100 ms is three times the
// longest; the fastest car covers about 100 m/s, i.e. 10 m in 100 ms, and 15 m leaves room for the car's
// own segment on top. A previous look older than that, or further away, is not the same drive.
//
// Hooks: the checkpoint test is overridden (sub_822653A8, below); the line test is a hook in the middle
// of sub_826881A8 (config/checkpoint_sweep.toml, mcla_trigger_line_sweep).

#include "generated/default/mcla_init.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>

#include "checkpoint_sweep_tracker.h"

// The game's own checkpoint test (the generated body), called by the override below.
REX_EXTERN(__imp__sub_822653A8);

namespace {

// scrThread::Run (sub_82554E20) stores the thread it runs here (0 outside a script); the thread's +16
// is the program counter of the opcode being run. The same two words pad_probe.cpp reads.
constexpr uint32_t kCurrentScriptThread = 0x828DA3BC;
constexpr uint32_t kScriptThreadPc = 16;

using mcla::sweep::Dot;
using mcla::sweep::LookTable;
using mcla::sweep::Sub;
using mcla::sweep::V3;

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
  const uint32_t bits = ReadU32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void WriteF32(uint8_t* base, uint32_t address, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  WriteU32(base, address, bits);
}

V3 ReadV3(const uint8_t* base, uint32_t addr) {
  return {ReadF32(base, addr), ReadF32(base, addr + 4), ReadF32(base, addr + 8)};
}

void WriteV3(uint8_t* base, uint32_t addr, const V3& v) {
  WriteF32(base, addr, v.x);
  WriteF32(base, addr + 4, v.y);
  WriteF32(base, addr + 8, v.z);
}

std::mutex g_mutex;
LookTable g_looks;
std::atomic<int> g_told[2];  // checkpoint, line trigger: outcomes changed or segments extended

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// This place's previous look of the car that is asked about, if it is recent and the segment [start,
// end] begins beyond it. Stores `end` as the car's new previous end either way. `gap_ms` is the time
// since the previous look (-1 if there was none).
bool PreviousEnd(uint32_t thread, uint32_t pc, uint32_t subject, uint32_t racer, const V3& start,
                 const V3& end, V3& previous, int64_t& gap_ms) {
  const int64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_looks.Update(thread, pc, subject, racer, start, end, now, previous, gap_ms);
}

}  // namespace

// The checkpoint test, called by the three natives above (and by the capture-the-flag natives, which
// test the same way). r3 = the checkpoint, r4 = the end of the segment, r5 = its start (16-byte
// vectors in guest memory); the answer is r3 = 0 or 1. The game's own test runs first; a look that
// finds nothing tests the piece of road since the script's previous look as well.
extern "C" REX_FUNC(sub_822653A8) {
  const uint32_t checkpoint = ctx.r3.u32;
  const uint32_t end_ptr = ctx.r4.u32;
  const uint32_t start_ptr = ctx.r5.u32;
  const auto lr = ctx.lr;
  __imp__sub_822653A8(ctx, base);

  const uint32_t thread = ReadU32(base, kCurrentScriptThread);
  if (!thread || end_ptr < 0x10000u || start_ptr < 0x10000u) {
    return;
  }
  const uint32_t pc = ReadU32(base, thread + kScriptThreadPc);
  const V3 start = ReadV3(base, start_ptr);
  const V3 end = ReadV3(base, end_ptr);
  V3 previous{};
  int64_t gap_ms = -1;
  const bool behind = PreviousEnd(thread, pc, checkpoint, 0, start, end, previous, gap_ms);
  if (!behind || (ctx.r3.u32 & 0xFFu) != 0) {
    return;
  }

  // The same test on [previous, end]. The scratch vector sits just under the caller's frame, and the
  // call runs a little lower still, so its frame does not reach it (the guest's stack below r1 is
  // free; the original does the same).
  const uint32_t sp = ctx.r1.u32;
  const uint32_t scratch = (sp - 0x80u) & ~15u;
  for (uint32_t i = 0; i < 16; ++i) {
    base[scratch + i] = base[start_ptr + i];
  }
  WriteV3(base, scratch, previous);
  ctx.r1.u32 = sp - 0x100u;
  ctx.lr = lr;
  ctx.r3.u64 = checkpoint;
  ctx.r4.u64 = end_ptr;
  ctx.r5.u64 = scratch;
  __imp__sub_822653A8(ctx, base);
  ctx.r1.u32 = sp;
  ctx.lr = lr;
  if ((ctx.r3.u32 & 0xFFu) != 0) {
    ctx.r3.u64 = 1;
    if (g_told[0].fetch_add(1) < 4) {
      REXLOG_INFO(
          "[CheckpointSweep] checkpoint {:08X}: the car crossed it between two looks of the script "
          "({} ms apart, {:.1f} m): it is told it was hit",
          checkpoint, gap_ms, std::sqrt(Dot(Sub(previous, start), Sub(previous, start))));
    }
  } else {
    ctx.r3.u64 = 0;
  }
}

// 0x82688200 in mcTriggerLine::Check (sub_826881A8; r27 = the racer, r31 = the trigger), before the
// `beq` that follows the two vector stores: the racer's previous position is on the stack at r1 + 128
// (the segment's start) and the car's position at r1 + 144 (its end). A script's look (not the trigger
// manager's own, which runs in every update outside any script) that finds the start beyond its
// previous end starts the segment there instead, which contains the game's.
void mcla_trigger_line_sweep(PPCRegister& r1, PPCRegister& r31, PPCRegister& r27) {
  auto* runtime = rex::Runtime::instance();
  uint8_t* const base = runtime ? runtime->virtual_membase() : nullptr;
  if (!base) {
    return;
  }
  const uint32_t thread = ReadU32(base, kCurrentScriptThread);
  if (!thread) {
    return;
  }
  const uint32_t pc = ReadU32(base, thread + kScriptThreadPc);
  const uint32_t start_ptr = r1.u32 + 128;
  const uint32_t end_ptr = r1.u32 + 144;
  const V3 start = ReadV3(base, start_ptr);
  const V3 end = ReadV3(base, end_ptr);
  V3 previous{};
  int64_t gap_ms = -1;
  if (!PreviousEnd(thread, pc, r31.u32, r27.u32, start, end, previous, gap_ms)) {
    return;
  }
  WriteV3(base, start_ptr, previous);
  if (g_told[1].fetch_add(1) < 4) {
    // CHECK: not seen in a running game; whether the longer segment changed the answer is not known here.
    REXLOG_INFO(
        "[CheckpointSweep] line trigger {:08X}: the script looked {} ms after its previous look; the "
        "line is tested from where the previous look ended ({:.1f} m before the car's last position)",
        r31.u32, gap_ms, std::sqrt(Dot(Sub(previous, start), Sub(previous, start))));
  }
}

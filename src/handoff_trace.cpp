// Hooks for the hand-off trace (the SDK's rex::handoff; patches/README.md, rexglue-handoff-trace.patch). Each one records
// a time-stamped event where the game's threads meet; config/handoff_trace.toml puts them in the guest code. With
// --handoff_trace off (the default) each is a thread-local access and a call that returns at once.
//
// The places (found in cpu-profile, launcher/dev/profiling-2026-10-26.md):
//   sub_82148A10   the update thread's wait for a free slot of the command ring (a loop of Sleep(0));
//   sub_82161210   ends by publishing a slot (the head word of the ring, 0x82836814 + 8 * ring);
//   sub_82144888   one round of the thread that frees the slots (sub_82160088 sets the word at +4);
//   sub_82305E38   the render thread's loop: waits on a semaphore for work, runs the frame, signals;
//   sub_82305D00   the update thread's start of the render thread's frame, sub_823057E8 its wait for the last one;
//   sub_822C0980   the frame function (what draws the frame), sub_82131008 its caller.
// The waits and signals themselves (the semaphores and events) are the SDK's own events, with the guest call chain.

#include "generated/default/mcla_init.h"

#include "handoff_trace.h"

using namespace mcla_handoff;

namespace {

thread_local uint32_t t_ring_turns = 0;
thread_local uint32_t t_pump_round = 0;
thread_local uint32_t t_render_loop = 0;

}  // namespace

// 0x82148A10, the first instruction of sub_82148A10(object r3, time r4).
void mcla_ho_ring_begin(PPCRegister& r3) {
  t_ring_turns = 0;
  MclaHandoffEvent(kRingBegin, r3.u32);
}

// 0x82148AA8, the head of the loop that polls the ring (cmplw r3,r26): one pass per Sleep(0).
void mcla_ho_ring_turn(PPCRegister& r3) {
  (void)r3;
  ++t_ring_turns;
}

// 0x82148AE8, where every way through the wait (no wait, a slot freed, the 1,000 microseconds up) arrives.
void mcla_ho_ring_end(PPCRegister& r3) {
  (void)r3;
  MclaHandoffEvent(kRingEnd, t_ring_turns, t_ring_turns ? 1 : 0);
}

// 0x82161880, after the last store of sub_82161210: the head of the ring (r10 = ring * 8) is r8, the slot published.
void mcla_ho_ring_advance(PPCRegister& r10, PPCRegister& r8) {
  MclaHandoffEvent(kRingAdvance, r10.u32, r8.u32);
}

// 0x82144888, the first instruction of sub_82144888, one round of the thread that frees the slots.
void mcla_ho_pump_round(PPCRegister& r3) {
  (void)r3;
  MclaHandoffEvent(kPumpRound, ++t_pump_round);
}

// 0x82160144, the instruction after the store of the new word at +4 (r11) in sub_82160088.
void mcla_ho_pump_free(PPCRegister& r11) {
  MclaHandoffEvent(kPumpFree, r11.u32);
}

// 0x82305E90, where the render thread's loop comes back to before its wait for work.
void mcla_ho_render_loop(PPCRegister& r31) {
  (void)r31;
  MclaHandoffEvent(kRenderLoop, ++t_render_loop);
}

// 0x822C0980, the first instruction of the frame function.
void mcla_ho_render_frame(PPCRegister& r3) {
  MclaHandoffEvent(kRenderFrame, r3.u32);
}

// 0x82305D00, the first instruction of the update thread's start of the render thread's frame (r3 = the render thread's object).
void mcla_ho_kick(PPCRegister& r3) {
  MclaHandoffEvent(kKick, r3.u32);
}

// 0x823057E8, the first instruction of the check for the render thread's previous frame.
void mcla_ho_wait_frame_done(PPCRegister& r3) {
  MclaHandoffEvent(kWaitFrameDone, r3.u32);
}

// 0x82131008, the first instruction of sub_82131008.
void mcla_ho_game_loop(PPCRegister& r3) {
  MclaHandoffEvent(kGameLoop, r3.u32);
}

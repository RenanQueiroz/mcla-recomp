// The game's events in the hand-off trace (the SDK's rex::handoff, rexglue-handoff-trace.patch; launcher/dev/handoff_trace.py
// reads the file). The SDK's events are below 100; these are the game's, recorded from the hooks in src/handoff_trace.cpp
// (config/handoff_trace.toml) and from frame_timing.cpp and poll_wait.cpp. Off unless --handoff_trace=true: each call is one
// function call and two loads.
#pragma once

#include <cstdint>

#include <rex/handoff_trace.h>

namespace mcla_handoff {

enum Event : uint16_t {
  // The engine timer (sub_821BDA90) ran, on the update thread: a = the pass's number on that thread.
  kFrameStart = 100,
  // The game's swap (VdSwap, mcla_present_interval): a = the swap's number.
  kSwap = 101,
  // The frame-rate limiter waits (src/frame_timing.cpp): a = 0 at the engine timer, 1 at the swap; b = microseconds.
  kLimiterBegin = 102,
  kLimiterEnd = 103,
  // sub_82148A10, the wait for a free slot of the command ring (the update thread): a = the object.
  kRingBegin = 110,
  // ... leaves the wait: a = turns of the polling loop, b = 1 when it did wait.
  kRingEnd = 112,
  // The update thread has published a slot (the last statement of sub_82161210): a = the ring's offset, b = the new head.
  kRingAdvance = 113,
  // The thread that frees the slots (sub_82144888) starts a round: a = the round's number.
  kPumpRound = 120,
  // ... frees a slot (sub_82160088 sets the word at +4): a = its new value.
  kPumpFree = 121,
  // ... waits for its next round (mcla_pump_wake_on_demand): a = the milliseconds.
  kPumpSleepBegin = 122,
  // ... a = 1 when the main thread ended the wait, 0 when the time ran out.
  kPumpSleepEnd = 123,
  // The main thread's ring wait set the event the pump thread waits on (the first time in each wait).
  kPumpWakeSet = 124,
  // The render thread's loop (sub_82305E38) comes to its wait for work: a = the iteration's number.
  kRenderLoop = 130,
  // The render thread's frame function (sub_822C0980) starts.
  kRenderFrame = 131,
  // The update thread starts the render thread's frame (sub_82305D00).
  kKick = 132,
  // The update thread's check for the render thread's previous frame (sub_823057E8).
  kWaitFrameDone = 133,
  // sub_82131008 (called by the loop of the thread that runs the frame function's callers).
  kGameLoop = 134,
};

}  // namespace mcla_handoff

inline void MclaHandoffEvent(uint16_t event, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
  rex::handoff::Record(event, a, b, c);
}

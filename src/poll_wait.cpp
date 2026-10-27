// The main thread's wait for a free slot in the game's command ring, without polling.
//
// The game keeps a ring of three slots per context at 0x82836814: a word at +0 that the main
// thread advances when it finishes writing a slot, and one at +4 that another guest thread
// (the one that runs sub_82144888) sets when it is done with one (sub_82160088, the `stw r11,4(r6)`
// at 0x82160140). Before the main thread writes into the next slot it waits for the ring not to be
// full (sub_82148A10): up to 1,000 microseconds on the engine's microsecond clock it calls
// sub_821C91B8(0), which is Sleep(0) (a SwitchToThread), reads the word at +4 and the clock, and
// goes round again. In the city that loop is 2.8 of the main thread's 6.4 ms of CPU a frame
// (cpu-profile, launcher/dev/profiling-2026-10-26.md), 30% of it in the kernel.
//
// With mcla_poll_wait the loop's Sleep(0) is replaced by a wait on the word: a short spin with
// the processor's pause hint, then WaitOnAddress until the word changes (the release above wakes
// it), for at most the time left of the loop's 1,000 microseconds, rounded up to whole
// milliseconds. The loop is otherwise the game's own: it reads the word and the clock after every
// wait, so a wake-up that never comes (a writer this does not know about) costs the wait's
// timeout and nothing else, and the game's own give-up after the 1,000 microseconds is as it was.

#include "generated/default/mcla_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(mcla_poll_wait, false, "MCLA",
                    "The main thread waits for a free slot of the game's command ring on the slot's "
                    "word (a spin, then WaitOnAddress, woken by the thread that frees the slot) "
                    "instead of calling Sleep(0) in a loop: about 2.8 ms of the 6.4 ms of CPU a frame "
                    "the main thread uses in the city, a third of it in the kernel. Off is the game's "
                    "own loop");
REXCVAR_DEFINE_INT32(mcla_poll_spin_us, 40, "MCLA",
                     "With mcla_poll_wait: microseconds the main thread spins on the slot's word "
                     "(with the pause hint) before it sleeps on it")
    .range(0, 2000);
REXCVAR_DEFINE_BOOL(mcla_poll_stats, false, "MCLA",
                    "Development: log how the main thread's waits for a ring slot ended, every 10 "
                    "seconds");

namespace {

#if defined(_WIN32)
using WaitOnAddressFn = BOOL(WINAPI*)(volatile VOID*, PVOID, SIZE_T, DWORD);
using WakeByAddressFn = VOID(WINAPI*)(PVOID);

struct Sync {
  WaitOnAddressFn wait = nullptr;
  WakeByAddressFn wake_all = nullptr;
  Sync() {
    HMODULE module = GetModuleHandleW(L"kernelbase.dll");
    if (!module) {
      module = LoadLibraryW(L"kernelbase.dll");
    }
    if (module) {
      wait = reinterpret_cast<WaitOnAddressFn>(reinterpret_cast<void*>(GetProcAddress(module, "WaitOnAddress")));
      wake_all =
          reinterpret_cast<WakeByAddressFn>(reinterpret_cast<void*>(GetProcAddress(module, "WakeByAddressAll")));
    }
  }
};

const Sync& GetSync() {
  static const Sync sync;
  return sync;
}
#endif

uint8_t* GuestBase() {
  static uint8_t* base = [] {
    auto* runtime = rex::Runtime::instance();
    return runtime ? runtime->virtual_membase() : nullptr;
  }();
  return base;
}

struct Stats {
  std::atomic<uint64_t> calls{0}, done_in_spin{0}, slept{0}, woken{0}, timed_out{0}, no_time_left{0};
  std::atomic<int64_t> last_log_ns{0};
};
Stats g_stats;

void MaybeLog() {
  using namespace std::chrono;
  const int64_t now = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  int64_t last = g_stats.last_log_ns.load(std::memory_order_relaxed);
  if (last == 0) {
    g_stats.last_log_ns.compare_exchange_strong(last, now);
    return;
  }
  if (now - last < 10'000'000'000LL || !g_stats.last_log_ns.compare_exchange_strong(last, now)) {
    return;
  }
  REXLOG_INFO(
      "poll wait: {} calls, {} ended in the spin, {} slept ({} woken by the release, {} timed out), {} with "
      "no time left",
      g_stats.calls.load(), g_stats.done_in_spin.load(), g_stats.slept.load(), g_stats.woken.load(),
      g_stats.timed_out.load(), g_stats.no_time_left.load());
}

uint32_t Swap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

}  // namespace

// 0x82148AB0 in sub_82148A10: `li r3,0` ahead of `bl sub_821C91B8` (Sleep(0)), inside the wait for
// a free slot. r31 = the context (the byte at +199 picks the ring), r30 = 0x82836814 (the rings),
// r29 = the word at +4 as the loop last read it, r26 = the loop's deadline and r3 = its last clock
// reading (microseconds). True skips the Sleep(0).
bool mcla_poll_wait(PPCRegister& r31, PPCRegister& r30, PPCRegister& r29, PPCRegister& r26,
                    PPCRegister& r3) {
#if defined(_WIN32)
  if (!REXCVAR_GET(mcla_poll_wait)) {
    return false;
  }
  uint8_t* base = GuestBase();
  const Sync& sync = GetSync();
  if (!base || !sync.wait) {
    return false;
  }
  // sub_82148A10's own index: rlwinm r9,r10,30,2,28 of the byte at +199.
  const uint32_t ring = (uint32_t(base[r31.u32 + 199]) >> 2) & 0x3FFFFFF8u;
  volatile uint32_t* word = reinterpret_cast<volatile uint32_t*>(base + r30.u32 + 4 + ring);
  uint32_t expected = Swap32(r29.u32);  // guest memory is big-endian
  const bool stats = REXCVAR_GET(mcla_poll_stats);
  if (stats) {
    g_stats.calls.fetch_add(1, std::memory_order_relaxed);
    MaybeLog();
  }
  // Spin.
  const int32_t spin_us = REXCVAR_GET(mcla_poll_spin_us);
  if (spin_us > 0) {
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    const int64_t spin_ticks = freq.QuadPart * spin_us / 1000000;
    for (;;) {
      for (int i = 0; i < 16; ++i) {
        if (*word != expected) {
          if (stats) {
            g_stats.done_in_spin.fetch_add(1, std::memory_order_relaxed);
          }
          return true;
        }
        YieldProcessor();
      }
      QueryPerformanceCounter(&now);
      if (now.QuadPart - start.QuadPart >= spin_ticks) {
        break;
      }
    }
  }
  // Sleep until the word changes, for as long as the loop has left (it gives up at its deadline).
  const int32_t left_us = static_cast<int32_t>(r26.u32 - r3.u32);
  if (left_us <= 0) {
    if (stats) {
      g_stats.no_time_left.fetch_add(1, std::memory_order_relaxed);
    }
    return true;  // the loop's next turn sees the deadline
  }
  const DWORD timeout_ms = static_cast<DWORD>(std::min<int32_t>((left_us + 999) / 1000, 2));
  if (stats) {
    g_stats.slept.fetch_add(1, std::memory_order_relaxed);
  }
  const BOOL changed = sync.wait(word, &expected, sizeof(expected), timeout_ms);
  if (stats) {
    (changed ? g_stats.woken : g_stats.timed_out).fetch_add(1, std::memory_order_relaxed);
  }
  return true;
#else
  (void)r31, (void)r30, (void)r29, (void)r26, (void)r3;
  return false;
#endif
}

// 0x82160140 in sub_82160088, after `stw r11,4(r6)`: r6 = 0x82836814, so the word at r6+4 has just
// been set; wake the main thread if it sleeps on it.
void mcla_poll_release(PPCRegister& r6) {
#if defined(_WIN32)
  if (!REXCVAR_GET(mcla_poll_wait)) {
    return;
  }
  uint8_t* base = GuestBase();
  const Sync& sync = GetSync();
  if (base && sync.wake_all) {
    sync.wake_all(base + r6.u32 + 4);
  }
#else
  (void)r6;
#endif
}

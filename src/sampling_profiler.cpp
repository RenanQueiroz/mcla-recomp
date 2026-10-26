// Built-in sampling profiler (development aid, Windows only for now).
//
// --mcla_profile=<file> starts a thread that, in a window, samples every other
// thread of the process about --mcla_profile_hz times a second (suspend, read the
// context, walk the host stack with the PE unwind tables, resume) and writes the
// stacks folded, with each thread's CPU time over the window, the number of
// swaps and engine frames in it, and the module table, for
// scripts/profile_report.py to symbolise.
//
// Windows:
//   - --mcla_profile_delay=N (default 40; -1 = none): one window of
//     --mcla_profile_seconds, N seconds after start, written to <file>;
//   - a pad script or the pad pipe sets mcla_profile_segment=<label> (`set
//     mcla_profile_segment roam`): a window starts then and is written to
//     <file stem>.<label><ext>. Several in one run, one at a time;
//   - --mcla_profile_hz=0: no sampling, only the threads' CPU time and the frame
//     counts over the window (no perturbation: for per-frame costs in A/B runs).
//   --mcla_profile_thread_log=<csv>: once a second, every thread's CPU time over
//   that second with the swap and update counts, for the whole run (always cheap).
//
// A frame of a recompiled game function is `g<guest address>` (the host
// function that contains the pc is looked up in the exe's .pdata and matched to
// the dispatch table, so host code that sits next to the generated code is not
// taken for a guest function); anything else is `<module index>:<rva>` (the
// function's start when it has unwind data, else the pc).
//
// Nothing may allocate or log, or take a lock, between SuspendThread and
// ResumeThread: the suspended thread could be holding the heap or the logger
// lock. Everything the suspended section touches is allocated beforehand.

#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

REXCVAR_DEFINE_STRING(mcla_profile, "", "MCLA", "Write a sampling profile to this file");
REXCVAR_DEFINE_INT32(mcla_profile_delay, 40, "MCLA",
                     "Seconds to wait before the first window (-1: only windows started by "
                     "mcla_profile_segment)")
    .range(-1, 3600);
REXCVAR_DEFINE_INT32(mcla_profile_seconds, 15, "MCLA", "Seconds to sample for").range(1, 600);
REXCVAR_DEFINE_INT32(mcla_profile_hz, 1000, "MCLA",
                     "Samples per second and thread (0: only thread CPU times and frame counts)")
    .range(0, 4000);
REXCVAR_DEFINE_INT32(mcla_profile_depth, 24, "MCLA", "Frames kept per sample (1: the pc only)")
    .range(1, 64);
REXCVAR_DEFINE_STRING(mcla_profile_segment, "", "MCLA",
                      "Setting this starts a profile window named by the value");
REXCVAR_DEFINE_STRING(mcla_profile_thread_log, "", "MCLA",
                      "Once a second, append every thread's CPU time and the swap and update "
                      "counts to this CSV");

// Defined in crash_trace.cpp.
uint32_t MclaGuestFunctionForHostPc(uintptr_t host_pc);
const std::vector<std::pair<uintptr_t, uint32_t>>& MclaGuestFunctionTable();

namespace {
std::atomic<uint64_t> g_swaps{0};
std::atomic<uint64_t> g_updates{0};
}  // namespace

// Counted from frame_timing.cpp: one per guest swap and one per engine timer pass.
void MclaProfileCountSwap() { g_swaps.fetch_add(1, std::memory_order_relaxed); }
void MclaProfileCountUpdate() { g_updates.fetch_add(1, std::memory_order_relaxed); }

#ifdef _WIN32
namespace {

struct ModuleInfo {
  uintptr_t base = 0;
  size_t size = 0;
  std::string path;
  const RUNTIME_FUNCTION* pdata = nullptr;
  uint32_t pdata_count = 0;
  bool system = false;  // ntdll, win32u, kernelbase, kernel32: where a blocked thread sits
};

// Replaced only by the sampler thread, outside the suspended section.
std::vector<ModuleInfo> g_modules;  // sorted by base
std::unordered_map<uint32_t, uint32_t> g_guest_by_function_rva;  // exe .pdata begin -> guest address
uintptr_t g_exe_base = 0;
std::atomic<bool> g_unknown_module_seen{false};

void RefreshModules() {
  std::vector<ModuleInfo> modules;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                             GetCurrentProcessId());
  if (snapshot == INVALID_HANDLE_VALUE) {
    return;
  }
  MODULEENTRY32 entry{};
  entry.dwSize = sizeof entry;
  if (Module32First(snapshot, &entry)) {
    do {
      ModuleInfo m;
      m.base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
      m.size = entry.modBaseSize;
      m.path = entry.szExePath;
      {
        std::string lower = entry.szModule;
        for (char& c : lower) {
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        m.system = lower == "ntdll.dll" || lower == "win32u.dll" || lower == "kernelbase.dll" ||
                   lower == "kernel32.dll";
      }
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
      if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.NumberOfRvaAndSizes >
                                                       IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
          const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
          if (dir.VirtualAddress && dir.Size) {
            m.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(m.base + dir.VirtualAddress);
            m.pdata_count = dir.Size / sizeof(RUNTIME_FUNCTION);
          }
        }
      }
      modules.push_back(std::move(m));
    } while (Module32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
  std::sort(modules.begin(), modules.end(),
            [](const ModuleInfo& a, const ModuleInfo& b) { return a.base < b.base; });
  g_modules = std::move(modules);
  g_unknown_module_seen.store(false, std::memory_order_relaxed);
}

const ModuleInfo* FindModule(uint64_t pc) {
  size_t lo = 0, hi = g_modules.size();
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (g_modules[mid].base + g_modules[mid].size <= pc) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < g_modules.size() && pc >= g_modules[lo].base) {
    return &g_modules[lo];
  }
  return nullptr;
}

const RUNTIME_FUNCTION* FindFunction(const ModuleInfo& m, uint32_t rva) {
  size_t lo = 0, hi = m.pdata_count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (m.pdata[mid].EndAddress <= rva) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < m.pdata_count && rva >= m.pdata[lo].BeginAddress) {
    return &m.pdata[lo];
  }
  return nullptr;
}

void BuildGuestMap() {
  g_guest_by_function_rva.clear();
  g_exe_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const ModuleInfo* exe = FindModule(g_exe_base);
  if (!exe) {
    return;
  }
  for (const auto& [host, guest] : MclaGuestFunctionTable()) {
    const ModuleInfo* m = FindModule(host);
    if (m != exe) {
      continue;
    }
    if (const RUNTIME_FUNCTION* rf = FindFunction(*exe, static_cast<uint32_t>(host - exe->base))) {
      g_guest_by_function_rva.emplace(rf->BeginAddress, guest);
    }
  }
}

using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

struct ThreadInfo {
  DWORD id = 0;
  HANDLE handle = nullptr;
  uint64_t stack_lo = 0;
  uint64_t stack_hi = 0;
  std::string name;
  uint64_t user_100ns = 0;
  uint64_t kernel_100ns = 0;
  uint64_t cycles = 0;
  uint64_t samples = 0;
  uint64_t reused = 0;
  // The last sample: while the thread's cycle count has not moved it has not run, and is still
  // there (a thread blocked in a wait), so the stack is counted again without suspending it.
  uint64_t last_cycles = 0;
  bool last_in_system = false;
  uint32_t last_offset = 0;
  uint32_t last_length = 0;
  bool have_last = false;
  bool dead = false;
};

uint64_t FileTimeValue(const FILETIME& ft) {
  return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

void ReadTimes(ThreadInfo& t, uint64_t* user, uint64_t* kernel, uint64_t* cycles) {
  FILETIME create, exit_time, kernel_time, user_time;
  *user = *kernel = *cycles = 0;
  if (GetThreadTimes(t.handle, &create, &exit_time, &kernel_time, &user_time)) {
    *user = FileTimeValue(user_time);
    *kernel = FileTimeValue(kernel_time);
  }
  ULONG64 c = 0;
  if (QueryThreadCycleTime(t.handle, &c)) {
    *cycles = c;
  }
}

std::string ThreadName(HANDLE handle) {
  static const auto get = reinterpret_cast<GetThreadDescriptionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),
                                             "GetThreadDescription")));
  std::string name;
  if (get) {
    PWSTR w = nullptr;
    if (SUCCEEDED(get(handle, &w)) && w) {
      int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
      if (n > 1) {
        name.resize(n - 1);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, name.data(), n, nullptr, nullptr);
      }
      LocalFree(w);
    }
  }
  for (char& c : name) {
    if (c == ' ' || c == '\t' || c == '\n') {
      c = '_';
    }
  }
  return name.empty() ? std::string("-") : name;
}

// Adds the threads of the process that are not known yet (not the calling one).
void AddNewThreads(std::vector<ThreadInfo>& threads, bool for_sampling) {
  static const auto query = reinterpret_cast<NtQueryInformationThreadFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                                             "NtQueryInformationThread")));
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return;
  }
  THREADENTRY32 entry{};
  entry.dwSize = sizeof entry;
  if (Thread32First(snapshot, &entry)) {
    do {
      if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) {
        continue;
      }
      bool known = false;
      for (const ThreadInfo& t : threads) {
        if (t.id == entry.th32ThreadID) {
          known = true;
          break;
        }
      }
      if (known) {
        continue;
      }
      DWORD access = THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION;
      if (for_sampling) {
        access |= THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT;
      }
      HANDLE h = OpenThread(access, FALSE, entry.th32ThreadID);
      if (!h) {
        continue;
      }
      ThreadInfo t;
      t.id = entry.th32ThreadID;
      t.handle = h;
      t.name = ThreadName(h);
      if (query) {
        struct {
          LONG exit_status;
          PVOID teb;
          PVOID pid;
          PVOID tid;
          ULONG_PTR affinity;
          LONG priority;
          LONG base_priority;
        } info{};
        if (query(h, 0, &info, sizeof info, nullptr) >= 0 && info.teb) {
          const auto* tib = static_cast<const NT_TIB*>(info.teb);
          t.stack_hi = reinterpret_cast<uint64_t>(tib->StackBase);
          t.stack_lo = reinterpret_cast<uint64_t>(tib->StackLimit);
        }
      }
      threads.push_back(std::move(t));
    } while (Thread32Next(snapshot, &entry));
  }
  CloseHandle(snapshot);
}

constexpr uint64_t kReuseCycles = 150000;

struct Rec {
  uint32_t thread;  // index into the thread list
  uint32_t offset;  // into the pool
  uint32_t length;
  uint32_t round;
};

// Walks the host stack of a suspended thread. Only reads memory of the thread's own stack
// (checked against its limits) and the modules' unwind data.
size_t Unwind(const CONTEXT& start, const ThreadInfo& t, uint64_t* out, size_t max_depth) {
  CONTEXT c = start;
  size_t n = 0;
  if (t.stack_hi == 0 || c.Rsp < t.stack_lo || c.Rsp >= t.stack_hi) {
    if (max_depth) {
      out[n++] = c.Rip;
    }
    return n;
  }
  uint64_t previous_rsp = c.Rsp;
  while (n < max_depth) {
    const uint64_t pc = c.Rip;
    if (!pc) {
      break;
    }
    // A return address points after the call; the function that contains the call is the one
    // that contains the byte before it.
    const uint64_t lookup = n == 0 ? pc : pc - 1;
    out[n++] = lookup;
    const ModuleInfo* m = FindModule(lookup);
    if (!m) {
      g_unknown_module_seen.store(true, std::memory_order_relaxed);
      break;
    }
    const RUNTIME_FUNCTION* rf = FindFunction(*m, static_cast<uint32_t>(lookup - m->base));
    if (!rf) {
      // No unwind data: a leaf function, the return address is at the top of the stack.
      if (c.Rsp + 8 > t.stack_hi) {
        break;
      }
      c.Rip = *reinterpret_cast<const uint64_t*>(c.Rsp);
      c.Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, lookup, const_cast<PRUNTIME_FUNCTION>(rf), &c,
                       &handler_data, &establisher, nullptr);
    }
    if (c.Rsp <= previous_rsp || c.Rsp >= t.stack_hi) {
      break;
    }
    previous_rsp = c.Rsp;
  }
  return n;
}

std::string SegmentPath(const std::string& path, const std::string& label) {
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of("\\/");
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    return path.substr(0, dot) + "." + label + path.substr(dot);
  }
  return path + "." + label;
}

std::string FrameKey(uint64_t pc) {
  const ModuleInfo* m = FindModule(pc);
  char buffer[64];
  if (!m) {
    std::snprintf(buffer, sizeof buffer, "?:%llx", static_cast<unsigned long long>(pc));
    return buffer;
  }
  uint32_t rva = static_cast<uint32_t>(pc - m->base);
  if (const RUNTIME_FUNCTION* rf = FindFunction(*m, rva)) {
    if (m->base == g_exe_base) {
      auto it = g_guest_by_function_rva.find(rf->BeginAddress);
      if (it != g_guest_by_function_rva.end()) {
        std::snprintf(buffer, sizeof buffer, "g%08X", it->second);
        return buffer;
      }
    }
    rva = rf->BeginAddress;
  }
  const size_t index = static_cast<size_t>(m - g_modules.data());
  std::snprintf(buffer, sizeof buffer, "%zu:%x", index, rva);
  return buffer;
}

void RunWindow(const std::string& out_path, const std::string& label, int seconds, int hz,
               int depth) {
  RefreshModules();
  BuildGuestMap();

  std::vector<ThreadInfo> threads;
  AddNewThreads(threads, hz > 0);
  for (ThreadInfo& t : threads) {
    ReadTimes(t, &t.user_100ns, &t.kernel_100ns, &t.cycles);
  }
  const uint64_t swaps0 = g_swaps.load(), updates0 = g_updates.load();

  // Capacity for every thread sampled every round of the window; a full buffer ends the
  // window early and says so.
  const size_t max_threads = 96;
  const size_t max_recs = hz > 0 ? static_cast<size_t>(seconds) * hz * max_threads : 0;
  const size_t max_pool = hz > 0 ? static_cast<size_t>(seconds) * hz * 40 * 12 : 0;
  std::vector<Rec> recs;
  std::vector<uint64_t> pool;
  recs.reserve(max_recs);
  pool.reserve(max_pool);
  threads.reserve(max_threads + 8);

  using Clock = std::chrono::steady_clock;
  const auto t_start = Clock::now();
  const auto end = t_start + std::chrono::seconds(seconds);
  uint64_t rounds = 0;
  uint64_t rng = 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(t_start.time_since_epoch().count());
  const double period_us = hz > 0 ? 1e6 / hz : 0;
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
  bool truncated = false;
  auto next_enumeration = t_start + std::chrono::milliseconds(250);
  uint64_t overhead_ns = 0;
  while (Clock::now() < end) {
    if (hz > 0) {
      const auto round_start = Clock::now();
      if (g_unknown_module_seen.load(std::memory_order_relaxed)) {
        RefreshModules();
        BuildGuestMap();
      }
      if (round_start >= next_enumeration && threads.size() < max_threads) {
        AddNewThreads(threads, true);
        next_enumeration = round_start + std::chrono::milliseconds(250);
        // A thread that started during the window is counted from its start (base 0).
      }
      if (recs.size() + threads.size() > max_recs ||
          pool.size() + threads.size() * static_cast<size_t>(depth) > max_pool) {
        truncated = true;
        break;
      }
      for (size_t i = 0; i < threads.size(); ++i) {
        ThreadInfo& t = threads[i];
        if (t.dead) {
          continue;
        }
        if (t.have_last) {
          ULONG64 cycles = 0;
          // Suspending a blocked thread costs it a few thousand cycles (the suspend APC); a thread
          // that woke up and did something costs more.
          if (t.last_in_system && QueryThreadCycleTime(t.handle, &cycles) &&
              cycles - t.last_cycles < kReuseCycles) {
            recs.push_back({static_cast<uint32_t>(i), t.last_offset, t.last_length,
                            static_cast<uint32_t>(rounds)});
            ++t.samples;
            ++t.reused;
            continue;
          }
        }
        if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
          t.dead = true;
          continue;
        }
        CONTEXT context;
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(t.handle, &context)) {
          const size_t before = pool.size();
          pool.resize(before + depth);  // within capacity: no allocation
          const size_t n = Unwind(context, t, pool.data() + before, static_cast<size_t>(depth));
          pool.resize(before + n);
          recs.push_back({static_cast<uint32_t>(i), static_cast<uint32_t>(before),
                          static_cast<uint32_t>(n), static_cast<uint32_t>(rounds)});
          ++t.samples;
          ULONG64 cycles = 0;
          t.have_last = QueryThreadCycleTime(t.handle, &cycles) != 0;
          t.last_cycles = cycles;
          {
            const ModuleInfo* leaf = n ? FindModule(pool[before]) : nullptr;
            t.last_in_system = leaf && leaf->system;
          }
          t.last_offset = static_cast<uint32_t>(before);
          t.last_length = static_cast<uint32_t>(n);
        }
        ResumeThread(t.handle);
      }
      ++rounds;
      overhead_ns +=
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - round_start).count();
      // Sleep the rest of the period, with a little jitter so the samples don't lock to a
      // periodic part of the game.
      rng ^= rng << 13;
      rng ^= rng >> 7;
      rng ^= rng << 17;
      const double jitter = (static_cast<double>(rng % 2001) - 1000.0) / 1000.0 * 0.15 * period_us;
      const double elapsed_us =
          std::chrono::duration<double, std::micro>(Clock::now() - round_start).count();
      const double sleep_us = period_us + jitter - elapsed_us;
      if (sleep_us > 50 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(sleep_us * 10);
        SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
        WaitForSingleObject(timer, 20);
      } else if (sleep_us > 50) {
        Sleep(1);
      }
    } else {
      Sleep(20);
    }
  }
  if (timer) {
    CloseHandle(timer);
  }
  const double wall = std::chrono::duration<double>(Clock::now() - t_start).count();
  const uint64_t swaps1 = g_swaps.load(), updates1 = g_updates.load();
  struct Delta {
    uint64_t user, kernel, cycles;
  };
  std::vector<Delta> deltas(threads.size());
  for (size_t i = 0; i < threads.size(); ++i) {
    uint64_t u, k, c;
    ReadTimes(threads[i], &u, &k, &c);
    deltas[i] = {u - threads[i].user_100ns, k - threads[i].kernel_100ns, c - threads[i].cycles};
  }

  // Fold: (thread, stack) -> id and hits.
  struct Hit {
    uint32_t id;
    uint32_t count;
  };
  std::map<std::pair<uint32_t, std::string>, Hit> hits;
  std::unordered_map<uint64_t, std::string> frame_cache;
  std::string stack;
  std::vector<uint32_t> rec_stack(recs.size());
  for (size_t ri = 0; ri < recs.size(); ++ri) {
    const Rec& r = recs[ri];
    stack.clear();
    for (uint32_t k = 0; k < r.length; ++k) {
      const uint64_t pc = pool[r.offset + k];
      auto it = frame_cache.find(pc);
      if (it == frame_cache.end()) {
        it = frame_cache.emplace(pc, FrameKey(pc)).first;
      }
      if (k) {
        stack += ';';
      }
      stack += it->second;
    }
    auto hit = hits.try_emplace({r.thread, stack}, Hit{static_cast<uint32_t>(hits.size()), 0}).first;
    ++hit->second.count;
    rec_stack[ri] = hit->second.id;
  }

  // The timeline of the busiest threads: per round, the stack id each was in.
  std::vector<size_t> busiest(threads.size());
  for (size_t i = 0; i < busiest.size(); ++i) {
    busiest[i] = i;
  }
  std::sort(busiest.begin(), busiest.end(),
            [&](size_t a, size_t b) { return deltas[a].cycles > deltas[b].cycles; });
  if (busiest.size() > 10) {
    busiest.resize(10);
  }
  std::vector<int> slot_of(threads.size(), -1);
  for (size_t k = 0; k < busiest.size(); ++k) {
    slot_of[busiest[k]] = static_cast<int>(k);
  }
  std::vector<std::vector<int64_t>> timeline(rounds, std::vector<int64_t>(busiest.size(), -1));
  for (size_t ri = 0; ri < recs.size(); ++ri) {
    const int slot = slot_of[recs[ri].thread];
    if (slot >= 0 && recs[ri].round < rounds) {
      timeline[recs[ri].round][slot] = rec_stack[ri];
    }
  }

  if (FILE* f = std::fopen(out_path.c_str(), "w")) {
    std::fprintf(f,
                 "# mcla-profile v3\n# label=%s seconds=%.3f hz=%d depth=%d rounds=%llu "
                 "samples=%zu swaps=%llu updates=%llu truncated=%d round_cost_us=%.1f\n",
                 label.c_str(), wall, hz, depth, static_cast<unsigned long long>(rounds),
                 recs.size(), static_cast<unsigned long long>(swaps1 - swaps0),
                 static_cast<unsigned long long>(updates1 - updates0), truncated ? 1 : 0,
                 rounds ? overhead_ns / 1000.0 / rounds : 0.0);
    for (size_t i = 0; i < g_modules.size(); ++i) {
      std::fprintf(f, "M\t%zu\t%llx\t%zx\t%s\n", i,
                   static_cast<unsigned long long>(g_modules[i].base), g_modules[i].size,
                   g_modules[i].path.c_str());
    }
    for (size_t i = 0; i < threads.size(); ++i) {
      std::fprintf(f, "T\t%u\t%s\t%.2f\t%.2f\t%llu\t%llu\t%llu\n", static_cast<unsigned>(threads[i].id),
                   threads[i].name.c_str(), deltas[i].user / 10000.0, deltas[i].kernel / 10000.0,
                   static_cast<unsigned long long>(deltas[i].cycles),
                   static_cast<unsigned long long>(threads[i].samples),
                   static_cast<unsigned long long>(threads[i].reused));
    }
    for (const auto& [key, hit] : hits) {
      std::fprintf(f, "S\t%u\t%u\t%u\t%s\n", static_cast<unsigned>(threads[key.first].id),
                   hit.count, hit.id, key.second.c_str());
    }
    std::fprintf(f, "JT");
    for (size_t k : busiest) {
      std::fprintf(f, "\t%u", static_cast<unsigned>(threads[k].id));
    }
    std::fprintf(f, "\n");
    for (size_t round = 0; round < timeline.size(); ++round) {
      std::fprintf(f, "J\t%zu", round);
      for (int64_t id : timeline[round]) {
        std::fprintf(f, "\t%lld", static_cast<long long>(id));
      }
      std::fprintf(f, "\n");
    }
    std::fclose(f);
  }
  for (ThreadInfo& t : threads) {
    CloseHandle(t.handle);
  }
  REXLOG_INFO("profile '{}': {} samples over {} rounds in {:.1f} s ({} swaps, {} updates) -> {}",
              label, recs.size(), rounds, wall, swaps1 - swaps0, updates1 - updates0, out_path);
}

void ThreadLog(std::string path) {
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    return;
  }
  std::fprintf(f, "t_ms,swaps,updates,tid,name,cpu_ms,cycles\n");
  std::vector<ThreadInfo> threads;
  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  std::unordered_map<DWORD, std::pair<uint64_t, uint64_t>> last;  // tid -> (cpu 100ns, cycles)
  for (;;) {
    Sleep(1000);
    AddNewThreads(threads, false);
    const auto t_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    const uint64_t swaps = g_swaps.load(), updates = g_updates.load();
    for (ThreadInfo& t : threads) {
      uint64_t u, k, c;
      ReadTimes(t, &u, &k, &c);
      auto& prev = last[t.id];
      const uint64_t cpu = u + k;
      if (cpu != prev.first || c != prev.second) {
        std::fprintf(f, "%lld,%llu,%llu,%u,%s,%.2f,%llu\n", static_cast<long long>(t_ms),
                     static_cast<unsigned long long>(swaps),
                     static_cast<unsigned long long>(updates), static_cast<unsigned>(t.id),
                     t.name.c_str(),
                     (cpu - prev.first) / 10000.0, static_cast<unsigned long long>(c - prev.second));
      }
      prev = {cpu, c};
    }
    std::fflush(f);
  }
}

void RunProfiler(std::string path, int delay_s) {
  if (delay_s >= 0) {
    Sleep(static_cast<DWORD>(delay_s) * 1000);
    RunWindow(path, "auto", static_cast<int>(REXCVAR_GET(mcla_profile_seconds)),
              static_cast<int>(REXCVAR_GET(mcla_profile_hz)),
              static_cast<int>(REXCVAR_GET(mcla_profile_depth)));
  }
  // Windows started from a script: the segment cvar gets a new, non-empty value.
  std::string seen = REXCVAR_GET(mcla_profile_segment);
  for (;;) {
    Sleep(25);
    std::string now = REXCVAR_GET(mcla_profile_segment);
    if (now.empty() || now == seen) {
      continue;
    }
    seen = now;
    RunWindow(SegmentPath(path, now), now, static_cast<int>(REXCVAR_GET(mcla_profile_seconds)),
              static_cast<int>(REXCVAR_GET(mcla_profile_hz)),
              static_cast<int>(REXCVAR_GET(mcla_profile_depth)));
  }
}

}  // namespace
#endif

void MclaApp::StartProfilerIfRequested() {
#ifdef _WIN32
  std::string path = REXCVAR_GET(mcla_profile);
  if (!path.empty()) {
    std::thread(RunProfiler, path, static_cast<int>(REXCVAR_GET(mcla_profile_delay))).detach();
  }
  std::string thread_log = REXCVAR_GET(mcla_profile_thread_log);
  if (!thread_log.empty()) {
    std::thread(ThreadLog, thread_log).detach();
  }
#endif
}

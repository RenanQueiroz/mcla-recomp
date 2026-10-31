// Fused multiply-adds on a CPU without FMA3 (fork issue #21).
//
// The recompiler turns the console's scalar fused multiply-adds (fmadds, fmsubs, fnmsubs, fnmadds and the double
// forms: 9,118 sites) into std::fma(double, double, double), and the SDK builds for x86-64 with -msse4.1 only, so
// each one is a call to the C library's fma (ucrtbase.dll with llvm-mingw). That function runs one vfmadd213sd when
// the CPU has FMA3 (about 6 to 13 cycles with the call) and a long software routine when it has not (an Ivy Bridge
// or older Intel CPU, an AMD CPU before Piledriver): 157 to 165 cycles on a Ryzen 7 9800X3D, measured. The game's
// audio mixer thread makes about 20,000 to 30,000 of these calls a round (estimated from sampling profiles).
//
// Most of them take operands that are floats held in doubles (a single-precision instruction's inputs are
// single-precision values). The product of two floats has at most 48 significant bits, so it is EXACT in a double,
// and the fused result is then just the one rounding of (a * b) + c: a double multiply and a double add give the same
// bits as the fused instruction, in every rounding mode. This file replaces the C library's fma for that case
// when the CPU has no FMA3 (the library's own flag, _get_FMA3_enable, says so), and passes everything else on:
// operands that are not floats, infinities, NaNs. On a CPU with FMA3 it passes everything on, as before.
// Off switch: --fma_float_fast=false (the C library's routine, as before). Windows, llvm-mingw, x86-64 only; the
// file is empty elsewhere. Takes effect at the next start.
//
// MCLA_FMA_STANDALONE builds the file without the SDK (the lab's microbenchmark and the equality test).
// (launcher/dev/fma-without-fma3-2026-10-31.md)

#if defined(__MINGW32__) && defined(__x86_64__)

#include <atomic>
#include <cstdint>
#include <cstring>

#include <emmintrin.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef MCLA_FMA_STANDALONE
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(fma_float_fast, true, "MCLA",
                    "On a CPU without FMA3 (Intel before Haswell, AMD before Piledriver), fused multiply-adds whose "
                    "operands are single-precision values are computed with a double multiply and add (the product "
                    "is exact, so the bits are the fused result's) instead of the C library's fma, whose software "
                    "routine takes 157 to 165 cycles on a fast core. Nothing changes on a CPU with FMA3. Off is the "
                    "library's fma. Takes effect at the next start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
#endif

#ifdef MCLA_LAB_FMA
// Lab build only (never merged): --mcla_lab_fma=nofma3 makes the C library believe the CPU has no FMA3
// (_set_FMA3_enable(0)), the path an Ivy Bridge takes; --mcla_lab_fma_count counts the calls per thread.
REXCVAR_DEFINE_STRING(mcla_lab_fma, "", "MCLA", "Lab: nofma3 = the C library's FMA3 path off (as on an Ivy Bridge)");
REXCVAR_DEFINE_BOOL(mcla_lab_fma_count, false, "MCLA", "Lab: count the fma calls per thread (the mixer's stat line shows them)");
#endif

extern "C" int _get_FMA3_enable(void);
extern "C" int _set_FMA3_enable(int);

namespace {

using FmaFn = double (*)(double, double, double);

constexpr int kUnset = 0;
constexpr int kLibrary = 1;   // the C library's fma (FMA3 CPUs, and the off switch)
constexpr int kExact = 2;     // float-valued operands without a library call, the rest to the library

std::atomic<int> g_mode{kUnset};
FmaFn g_library = nullptr;

#ifdef MCLA_LAB_FMA
bool g_count = false;
thread_local uint64_t t_calls = 0;
thread_local uint64_t t_eligible = 0;
#endif

FmaFn ResolveLibrary() {
  HMODULE m = GetModuleHandleW(L"ucrtbase.dll");
  if (!m) {
    m = LoadLibraryW(L"ucrtbase.dll");
  }
  FmaFn f = m ? reinterpret_cast<FmaFn>(GetProcAddress(m, "fma")) : nullptr;
  if (!f) {
    __builtin_trap();  // nothing sensible to fall back to: the library has no fma
  }
  return f;
}

inline bool Finite(double x) {
  uint64_t u;
  std::memcpy(&u, &x, sizeof(u));
  return ((u >> 52) & 0x7FF) != 0x7FF;
}

// x is exactly a single-precision value (and finite). A float denormal held in a double is a normal double.
inline bool FloatValued(double x) {
  return Finite(x) && static_cast<double>(static_cast<float>(x)) == x;
}

// a * b + c with the product exact: the one rounding of the fused operation. False if the operands are not
// of the kind (the caller then asks the library).
inline bool Exact(double a, double b, double c, double* out) {
  if (!(FloatValued(a) & FloatValued(b) & Finite(c))) {
    return false;
  }
  // _mm_mul_sd and _mm_add_sd, so the compiler cannot contract or reassociate them.
  const __m128d p = _mm_mul_sd(_mm_set_sd(a), _mm_set_sd(b));
  *out = _mm_cvtsd_f64(_mm_add_sd(p, _mm_set_sd(c)));
  return true;
}

int InitMode() {
#ifdef MCLA_LAB_FMA
  if (REXCVAR_GET(mcla_lab_fma) == "nofma3") {
    _set_FMA3_enable(0);
  }
  g_count = REXCVAR_GET(mcla_lab_fma_count);
#endif
  if (!g_library) {
    g_library = ResolveLibrary();
  }
  const bool fma3 = _get_FMA3_enable() != 0;
#ifdef MCLA_FMA_STANDALONE
  const bool fast = true;
#else
  const bool fast = REXCVAR_GET(fma_float_fast);
#endif
  const int mode = (!fma3 && fast) ? kExact : kLibrary;
  g_mode.store(mode, std::memory_order_release);
#ifndef MCLA_FMA_STANDALONE
  REXLOG_INFO("FMA: the C library's fma {}; {}", fma3 ? "uses FMA3 (this CPU has it)" : "runs its software routine (this CPU has no FMA3)",
              mode == kExact ? "single-precision operands are computed without the library (fma_float_fast)"
                             : (fma3 ? "nothing to change" : "fma_float_fast is off"));
#endif
  return mode;
}

}  // namespace

#ifdef MCLA_FMA_TEST
// The equality test (lab): the fast path alone, false if it does not apply.
extern "C" bool mcla_fma_exact(double a, double b, double c, double* out) { return Exact(a, b, c, out); }
#endif

#ifdef MCLA_LAB_FMA
uint64_t MclaFmaThreadCalls() { return t_calls; }
uint64_t MclaFmaThreadEligible() { return t_eligible; }
bool MclaFmaCounting() { return g_count; }
#endif

// The symbol the generated code's std::fma calls resolve to: a definition in the executable wins over the import
// library's thunk. Under another C++ name with an assembler label, so the library headers' declaration of fma (and
// its exception specification) cannot clash with it.
extern "C" double mcla_fma_symbol(double a, double b, double c) __asm__("fma");
extern "C" double mcla_fma_symbol(double a, double b, double c) {
  int mode = g_mode.load(std::memory_order_acquire);
  if (mode == kUnset) {
    mode = InitMode();
  }
#ifdef MCLA_LAB_FMA
  if (g_count) {
    ++t_calls;
    double ignored;
    if (Exact(a, b, c, &ignored)) {
      ++t_eligible;
    }
  }
#endif
  if (mode == kExact) {
    double r;
    if (Exact(a, b, c, &r)) {
      return r;
    }
  }
  return g_library(a, b, c);
}

#endif  // __MINGW32__ && __x86_64__

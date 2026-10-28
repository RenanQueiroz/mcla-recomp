// Host-side exactness test of the MXCSR changes in the code generator (codegen options csr_mode_free_ops and
// csr_cheap_switch; patches/rexglue-codegen-csr-lazy.patch). No game data; it runs anywhere on x86-64.
//
//   csr_exact [--scale N] [--threads T] [--exhaustive] [--quick]   (--quick: a few seconds, for a desk check)
//
// Part A. The scalar instructions that csr_mode_free_ops lets run in whatever flush mode the code is in: fmr, fabs, fnabs,
//   fneg, lfd (and the other lfd*), stfd (and the other stfd*), stfiwx, fctidz, fctiwz, fcfid. The C++ each one compiles to is
//   written here as the generator writes it (the SDK's src/codegen/builders/floating_point.cpp and memory.cpp). Every input is
//   run in "FPU" mode (denormals kept, MXCSR 0x1F80 | rounding) and in "VMX" mode (FTZ and DAZ set, MXCSR | 0x8040), for
//   each of the four rounding modes, and the results are compared bit for bit: any difference is a failure. Where the result is
//   defined without the hardware (the moves, the two truncating conversions) it is also compared with an integer-only
//   reference. Inputs: every sign x every biased exponent (2048) x a set of mantissas (zero, ones, each single bit, every
//   run of ones from the top and from the bottom, and 2^14 x scale random ones); for the denormal exponent (0), 2^22 x scale
//   more. fcfid: every 64-bit integer with at most two bits set, +-2^k +- 64, every run of ones, and random ones; with
//   --exhaustive every integer of 32 bits as well.
// Part A' (the control). The instructions that STAY in FPU mode (lfs, frsp, fadd, fcmpu, fsel, fctid with rounding up, fres)
//   are run the same way and MUST differ between the modes for some input: a test that finds no difference there could not
//   have found one in Part A. (lfs: with --exhaustive every one of the 2^32 float bit patterns.)
// Part B. The cheap switch: FPSCRRegister::{enable,disable}FlushMode{Fast,CheckedFast} against the old
//   {enable,disable}FlushMode{Unconditional,} for every rounding mode, every flush-bit pattern, with the exception masks set
//   and clear and the flag bits set and clear in the shadow: the shadow is equal afterwards, and so is the real MXCSR except
//   for its sticky flag bits (0-5), which nothing reads.
//
// Exit code 0: every check passed. Output: one line per check with the number of inputs and of differences.
#include <rex/ppc/context.h>

#include <cpuid.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Reg = PPCRegister;
using u64 = uint64_t;
using u32 = uint32_t;
#define NOINL __attribute__((noinline))

// ---- the instructions, as the generator writes them (the operand registers are d and a; b and c where there are more) ----
NOINL static void op_fabs(Reg& d, Reg& a, Reg&, Reg&) { d.u64 = a.u64 & ~0x8000000000000000; }
NOINL static void op_fnabs(Reg& d, Reg& a, Reg&, Reg&) { d.u64 = a.u64 | 0x8000000000000000; }
NOINL static void op_fneg(Reg& d, Reg& a, Reg&, Reg&) { d.u64 = a.u64 ^ 0x8000000000000000; }
NOINL static void op_fmr(Reg& d, Reg& a, Reg&, Reg&) { d.f64 = a.f64; }
NOINL static void op_fcfid(Reg& d, Reg& a, Reg&, Reg&) { d.f64 = double(a.s64); }
NOINL static void op_fctidz(Reg& d, Reg& a, Reg&, Reg&) {
  d.s64 = std::isnan(a.f64)         ? int64_t(0x8000000000000000ULL)
          : (a.f64 > double(LLONG_MAX)) ? LLONG_MAX
                                        : simde_mm_cvttsd_si64(simde_mm_load_sd(&a.f64));
}
NOINL static void op_fctiwz(Reg& d, Reg& a, Reg&, Reg&) {
  d.s64 = std::isnan(a.f64)          ? int64_t(0x80000000U)
          : (a.f64 >= double(INT_MAX)) ? INT_MAX
                                       : simde_mm_cvttsd_si32(simde_mm_load_sd(&a.f64));
}
// lfd / lfdx / lfdu / lfdux: f.u64 = REX_LOAD_U64(ea): the guest memory is big-endian, so the bytes are swapped on the way in.
static thread_local volatile u64 g_mem64;
NOINL static void op_lfd(Reg& d, Reg& a, Reg&, Reg&) {
  g_mem64 = __builtin_bswap64(a.u64);
  d.u64 = __builtin_bswap64(*(volatile u64*)&g_mem64);
}
// stfd* : REX_STORE_U64(ea, f.u64) and back; stfiwx: REX_STORE_U32(ea, f.u32) (the low word)
NOINL static void op_stfd(Reg& d, Reg& a, Reg&, Reg&) {
  *(volatile u64*)&g_mem64 = __builtin_bswap64(a.u64);
  d.u64 = __builtin_bswap64(g_mem64);
}
static thread_local volatile u32 g_mem32;
NOINL static void op_stfiwx(Reg& d, Reg& a, Reg&, Reg&) {
  *(volatile u32*)&g_mem32 = __builtin_bswap32(a.u32);
  d.u64 = __builtin_bswap32(g_mem32);
}
// ---- the controls: instructions that stay in FPU mode ----
NOINL static void op_lfs(Reg& d, Reg& a, Reg&, Reg&) {
  Reg temp;
  temp.u32 = a.u32;
  d.f64 = double(temp.f32);
}
NOINL static void op_frsp(Reg& d, Reg& a, Reg&, Reg&) { d.f64 = double(float(a.f64)); }
NOINL static void op_fadd(Reg& d, Reg& a, Reg& b, Reg&) { d.f64 = a.f64 + b.f64; }
NOINL static void op_fcmpu(Reg& d, Reg& a, Reg& b, Reg&) {
  PPCCRRegister cr{};
  cr.compare(a.f64, b.f64);
  d.u64 = cr.lt | (cr.gt << 1) | (cr.eq << 2) | (cr.un << 3);
}
NOINL static void op_fsel(Reg& d, Reg& a, Reg& b, Reg& c) { d.f64 = a.f64 >= 0.0 ? b.f64 : c.f64; }
NOINL static void op_fctid(Reg& d, Reg& a, Reg&, Reg&) {
  d.s64 = std::isnan(a.f64)         ? int64_t(0x8000000000000000ULL)
          : (a.f64 > double(LLONG_MAX)) ? LLONG_MAX
                                        : simde_mm_cvtsd_si64(simde_mm_load_sd(&a.f64));
}
NOINL static void op_fres(Reg& d, Reg& a, Reg&, Reg&) { d.f64 = double(float(1.0 / a.f64)); }

// ---- integer-only references (what the result is, in either mode) ----
static u64 ref_fctidz(u64 b) {
  const u64 sign = b >> 63, mant = b & ((1ULL << 52) - 1);
  const int e = int((b >> 52) & 0x7FF);
  if (e == 2047 && mant) return 0x8000000000000000ULL;                        // NaN
  if (!sign && (e > 0x43E || (e == 0x43E && mant) || e == 2047)) return u64(LLONG_MAX);  // above 2^63
  if (e < 1023) return 0;                                                       // |x| < 1: zero, denormals included
  if (e >= 1023 + 63) return 0x8000000000000000ULL;                             // out of range: the integer indefinite
  const u64 m = (1ULL << 52) | mant;
  const int sh = e - 1075;
  const u64 mag = sh >= 0 ? (m << sh) : (m >> -sh);
  return sign ? (0 - mag) : mag;
}
static u64 ref_fctiwz(u64 b) {
  const u64 sign = b >> 63, mant = b & ((1ULL << 52) - 1);
  const int e = int((b >> 52) & 0x7FF);
  if (e == 2047 && mant) return u64(int64_t(0x80000000U));  // NaN: 2147483648, as the generator writes it
  // x >= 2147483647.0 gives INT_MAX: exponent 31 or more, or exponent 30 with the top 30 mantissa bits all ones
  const bool ge_intmax = !sign && (e >= 1023 + 31 || (e == 1023 + 30 && (mant >> 22) == (1ULL << 30) - 1));
  if (ge_intmax) return u64(int64_t(INT_MAX));
  if (e < 1023) return 0;                                    // |x| < 1: zero, denormals included
  if (e >= 1023 + 31) return u64(int64_t(INT_MIN));          // a negative x of 2^31 or more: INT_MIN (exact, or the integer indefinite)
  const u64 m = (1ULL << 52) | mant;
  const int sh = e - 1075;
  const u64 mag = sh >= 0 ? (m << sh) : (m >> -sh);
  const int64_t v = sign ? -int64_t(mag) : int64_t(mag);
  return u64(int64_t(int32_t(v)));
}

// ---- machinery ----
typedef void (*OpFn)(Reg&, Reg&, Reg&, Reg&);
struct Op {
  const char* name;
  OpFn fn;
  bool free_op;          // part A (must not differ) or control (must differ for some input)
  u64 (*ref)(u64);       // an integer-only reference, or null
  int kind;              // 0 doubles, 1 floats (the low word), 2 integers (fcfid)
  int ninputs;           // 1, or 2/3 (the control ops take pairs/triples built from the input list)
};

static const u32 kFpu[4] = {0x1F80, 0x1F80 | (1 << 13), 0x1F80 | (2 << 13), 0x1F80 | (3 << 13)};
static const u32 kVmx[4] = {0x1F80 | 0x8040, 0x1F80 | 0x8040 | (1 << 13), 0x1F80 | 0x8040 | (2 << 13), 0x1F80 | 0x8040 | (3 << 13)};

struct Stats {
  std::atomic<u64> inputs{0}, diff_modes{0}, diff_ref{0}, diff_rc{0};
  std::atomic<u64> first[3] = {0, 0, 0};
  std::atomic<int> nfirst{0};
};

// Runs fn over xs in all eight (rounding, mode) combinations and compares.
static void run_batch(const Op& op, const std::vector<u64>& xs, Stats& st) {
  const size_t n = xs.size();
  static thread_local std::vector<u64> out[8];
  for (auto& o : out) o.assign(n, 0);
  for (int rc = 0; rc < 4; ++rc) {
    for (int vmx = 0; vmx < 2; ++vmx) {
      _mm_setcsr(vmx ? kVmx[rc] : kFpu[rc]);
      std::vector<u64>& o = out[rc * 2 + vmx];
      for (size_t i = 0; i < n; ++i) {
        Reg d, a, b, c;
        d.u64 = 0;
        a.u64 = xs[i];
        // the control ops with more than one input: b and c from neighbouring inputs
        b.u64 = xs[(i + 1) % n];
        c.u64 = xs[(i + 2) % n];
        op.fn(d, a, b, c);
        o[i] = d.u64;
      }
    }
  }
  _mm_setcsr(0x1F80);
  u64 dm = 0, dr = 0, drc = 0;
  for (size_t i = 0; i < n; ++i) {
    bool bad = false;
    for (int rc = 0; rc < 4; ++rc) {
      if (out[rc * 2][i] != out[rc * 2 + 1][i]) { ++dm; bad = true; break; }
    }
    if (op.ref && out[0][i] != op.ref(xs[i])) { ++dr; bad = true; }
    for (int k = 2; k < 8; ++k) {
      if (out[k & 1][i] != out[k][i]) { ++drc; break; }  // rounding-dependent (fcfid, fctid): informational
    }
    if (bad && op.free_op) {
      int idx = st.nfirst.fetch_add(1);
      if (idx < 3) st.first[idx] = xs[i];
    }
  }
  st.inputs += n;
  st.diff_modes += dm;
  st.diff_ref += dr;
  st.diff_rc += drc;
}

static u64 rng_state_next(u64& s) {
  s += 0x9E3779B97F4A7C15ULL;
  u64 z = s;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static std::vector<u64> structured_mantissas() {
  std::vector<u64> m;
  const u64 full = (1ULL << 52) - 1;
  m.push_back(0);
  m.push_back(full);
  m.push_back(1ULL << 51);
  for (int k = 0; k < 52; ++k) {
    m.push_back(1ULL << k);
    m.push_back(full & ~(1ULL << k));
    m.push_back((full << k) & full);      // run of ones from the top
    m.push_back(full >> k);               // run of ones from the bottom
  }
  return m;
}

static bool g_quick = false;

static void run_doubles(const Op& op, int tid, int nthreads, int scale, Stats& st) {
  const std::vector<u64> sm = structured_mantissas();
  u64 rng = 0x1234567ULL + 977 * tid;
  std::vector<u64> xs;
  for (int sign = 0; sign < 2; ++sign) {
    for (int e = tid; e < 2048; e += nthreads) {
      xs.clear();
      for (u64 m : sm) xs.push_back((u64(sign) << 63) | (u64(e) << 52) | m);
      const int randoms = g_quick ? (e == 0 ? (1 << 14) : 256) : (e == 0 ? (1 << 22) : (1 << 14)) * scale;
      for (int i = 0; i < randoms; ++i) xs.push_back((u64(sign) << 63) | (u64(e) << 52) | (rng_state_next(rng) & ((1ULL << 52) - 1)));
      run_batch(op, xs, st);
    }
  }
}

static void run_floats(const Op& op, int tid, int nthreads, int scale, bool exhaustive, Stats& st) {
  std::vector<u64> xs;
  if (exhaustive) {
    // every float bit pattern, in blocks
    const u64 total = 1ULL << 32;
    const u64 block = 1ULL << 20;
    for (u64 base = tid * block; base < total; base += u64(nthreads) * block) {
      xs.clear();
      for (u64 i = 0; i < block; ++i) xs.push_back(base + i);
      run_batch(op, xs, st);
    }
    return;
  }
  u64 rng = 0xABCDEFULL + 31 * tid;
  for (int sign = 0; sign < 2; ++sign) {
    for (int e = tid; e < 256; e += nthreads) {
      xs.clear();
      for (int i = 0; i < (g_quick ? 1024 : (1 << 16) * scale); ++i) xs.push_back((u64(sign) << 31) | (u64(e) << 23) | (rng_state_next(rng) & 0x7FFFFF));
      run_batch(op, xs, st);
    }
  }
}

static void run_ints(const Op& op, int tid, int nthreads, int scale, bool exhaustive, Stats& st) {
  std::vector<u64> xs;
  auto flush = [&] { if (!xs.empty()) { run_batch(op, xs, st); xs.clear(); } };
  if (tid == 0) {
    for (int a = 0; a < 64; ++a) {
      xs.push_back(1ULL << a);
      xs.push_back(0 - (1ULL << a));
      for (int b = a + 1; b < 64; ++b) xs.push_back((1ULL << a) | (1ULL << b));
      for (int d = -64; d <= 64; ++d) { xs.push_back((1ULL << a) + u64(int64_t(d))); xs.push_back(0 - ((1ULL << a) + u64(int64_t(d)))); }
      xs.push_back(~0ULL >> a);
      xs.push_back(~0ULL << a);
    }
    flush();
  }
  u64 rng = 0x55AA55ULL + 17 * tid;
  for (int i = 0; i < (g_quick ? (1 << 16) : (1 << 20) * scale); i += 1 << 16) {
    for (int j = 0; j < (1 << 16); ++j) xs.push_back(rng_state_next(rng));
    flush();
  }
  if (exhaustive) {
    const u64 total = 1ULL << 32, block = 1ULL << 20;
    for (u64 base = tid * block; base < total; base += u64(nthreads) * block) {
      for (u64 i = 0; i < block; ++i) xs.push_back(u64(int64_t(int32_t(u32(base + i)))));
      flush();
    }
  }
}

static std::string cpu_brand() {
  unsigned r[12] = {};
  if (__get_cpuid_max(0x80000000, nullptr) >= 0x80000004) {
    for (unsigned i = 0; i < 3; ++i) __get_cpuid(0x80000002 + i, &r[4 * i], &r[4 * i + 1], &r[4 * i + 2], &r[4 * i + 3]);
  }
  char b[49] = {};
  std::memcpy(b, r, 48);
  return b;
}

// ---- part B ----
static int part_b() {
  int fails = 0;
  u64 checks = 0;
  const u32 masks[2] = {0x1F80, 0};
  const u32 flags[3] = {0, 0x3F, 0x15};
  const u32 flushes[4] = {0, 0x8000, 0x0040, 0x8040};
  for (int rc = 0; rc < 4; ++rc)
    for (u32 m : masks)
      for (u32 f : flags)
        for (u32 fl : flushes)
          for (int dir = 0; dir < 2; ++dir)       // 0 enable, 1 disable
            for (int checked = 0; checked < 2; ++checked) {
              const u32 shadow = (u32(rc) << 13) | m | f | fl;
              PPCFPSCRRegister a, b;
              a.csr = b.csr = shadow;
              _mm_setcsr(0x1F80);
              if (dir == 0) { if (checked) a.enableFlushMode(); else a.enableFlushModeUnconditional(); }
              else          { if (checked) a.disableFlushMode(); else a.disableFlushModeUnconditional(); }
              const u32 real_a = _mm_getcsr();
              _mm_setcsr(0x1F80);
              if (dir == 0) { if (checked) b.enableFlushModeCheckedFast(); else b.enableFlushModeFast(); }
              else          { if (checked) b.disableFlushModeCheckedFast(); else b.disableFlushModeFast(); }
              const u32 real_b = _mm_getcsr();
              ++checks;
              // the checked forms leave the MXCSR alone when the shadow already says so: both then keep 0x1F80
              if (a.csr != b.csr || (real_a & ~0x3Fu) != (real_b & ~0x3Fu)) {
                if (fails < 5)
                  std::printf("  cheap switch MISMATCH rc=%d masks=%#x flags=%#x flush=%#x dir=%d checked=%d: shadow %#x/%#x mxcsr %#x/%#x\n", rc, m, f, fl, dir,
                              checked, a.csr, b.csr, real_a, real_b);
                ++fails;
              }
            }
  _mm_setcsr(0x1F80);
  std::printf("part B: cheap switch against the old calls: %llu cases, %d differences\n", (unsigned long long)checks, fails);
  return fails;
}

int main(int argc, char** argv) {
  int scale = 1, threads = 4;
  bool exhaustive = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--scale" && i + 1 < argc) scale = std::max(1, std::atoi(argv[++i]));
    else if (a == "--threads" && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
    else if (a == "--exhaustive") exhaustive = true;
    else if (a == "--quick") g_quick = true;
    else { std::printf("usage: csr_exact [--scale N] [--threads T] [--exhaustive]\n"); return 2; }
  }
  const auto t0 = std::chrono::steady_clock::now();
  std::printf("csr_exact: scale %d, %d threads%s; MXCSR before: %#x; CPU: %s\n", scale, threads, exhaustive ? ", exhaustive" : "", _mm_getcsr(),
              cpu_brand().c_str());
  const Op ops[] = {
      {"fabs", op_fabs, true, [](u64 b) { return b & ~(1ULL << 63); }, 0, 1},
      {"fnabs", op_fnabs, true, [](u64 b) { return b | (1ULL << 63); }, 0, 1},
      {"fneg", op_fneg, true, [](u64 b) { return b ^ (1ULL << 63); }, 0, 1},
      {"fmr", op_fmr, true, [](u64 b) { return b; }, 0, 1},
      {"lfd*", op_lfd, true, [](u64 b) { return b; }, 0, 1},
      {"stfd*", op_stfd, true, [](u64 b) { return b; }, 0, 1},
      {"stfiwx", op_stfiwx, true, [](u64 b) { return u64(u32(b)); }, 0, 1},
      {"fctidz", op_fctidz, true, ref_fctidz, 0, 1},
      {"fctiwz", op_fctiwz, true, ref_fctiwz, 0, 1},
      {"fcfid", op_fcfid, true, nullptr, 2, 1},
      // controls: these stay in FPU mode, and must show a difference
      {"lfs", op_lfs, false, nullptr, 1, 1},
      {"frsp", op_frsp, false, nullptr, 0, 1},
      {"fadd", op_fadd, false, nullptr, 0, 2},
      {"fcmpu", op_fcmpu, false, nullptr, 0, 2},
      {"fsel", op_fsel, false, nullptr, 0, 3},
      {"fctid", op_fctid, false, nullptr, 0, 1},
      {"fres", op_fres, false, nullptr, 0, 1},
  };
  int failures = 0;
  for (const Op& op : ops) {
    Stats st;
    const auto s0 = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
      pool.emplace_back([&, t] {
        if (op.kind == 0) run_doubles(op, t, threads, scale, st);
        else if (op.kind == 1) run_floats(op, t, threads, scale, exhaustive, st);
        else run_ints(op, t, threads, scale, exhaustive, st);
      });
    }
    for (auto& th : pool) th.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
    const bool is_free = op.free_op;
    const bool ok = is_free ? (st.diff_modes == 0 && st.diff_ref == 0) : (st.diff_modes > 0);
    std::printf("%-7s %s inputs %12llu  FPU-vs-VMX differences %10llu  reference differences %6llu  (rounding-dependent inputs %llu)  %.1f s  %s\n", op.name,
                is_free ? "[free]   " : "[control]", (unsigned long long)st.inputs.load(), (unsigned long long)st.diff_modes.load(),
                (unsigned long long)st.diff_ref.load(), (unsigned long long)st.diff_rc.load(), secs,
                ok ? "OK" : (is_free ? "FAIL: the modes differ" : "FAIL: the control shows no difference (the test is blind)"));
    if (!ok && is_free && st.nfirst > 0)
      std::printf("        first inputs: %#llx %#llx %#llx\n", (unsigned long long)st.first[0].load(), (unsigned long long)st.first[1].load(),
                  (unsigned long long)st.first[2].load());
    if (!ok) ++failures;
  }
  failures += part_b() ? 1 : 0;
  const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("csr_exact: %s (%d failed checks) in %.1f s\n", failures ? "FAILED" : "PASSED", failures, total);
  return failures ? 1 : 0;
}

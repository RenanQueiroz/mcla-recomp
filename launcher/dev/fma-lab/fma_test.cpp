// Equality test of the fast path in src/fma_fast.cpp against the hardware fused multiply-add of this CPU (vfmadd213sd),
// bit for bit, in the four rounding modes with FTZ/DAZ off and on; and its speed. Needs FMA3 (the reference).
// Built like the game (llvm-mingw, -msse4.1) with the file under test included:  -DMCLA_FMA_STANDALONE -DMCLA_FMA_TEST
// usage: fma_test.exe [millions of cases per setting]
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>

#include "../../../src/fma_fast.cpp"

__attribute__((target("fma"))) static double hw_fma(double a, double b, double c) {
  __m128d r = _mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c));
  return _mm_cvtsd_f64(r);
}

static uint64_t s0 = 0x9E3779B97F4A7C15ull, s1 = 0xD1B54A32D192ED03ull;
static inline uint64_t rnd64() {  // xorshift128+
  uint64_t x = s0, y = s1;
  s0 = y;
  x ^= x << 23;
  s1 = x ^ y ^ (x >> 17) ^ (y >> 26);
  return s1 + y;
}
static inline double from_bits(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
static inline uint64_t to_bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
static inline double rnd_float_valued() {
  uint32_t u = (uint32_t)rnd64();
  float f;
  std::memcpy(&f, &u, 4);
  return (double)f;   // NaN and inf possible: the fast path declines them
}
// floats of a moderate range, to get many results that are neither 0 nor huge
static inline double rnd_float_moderate() {
  uint32_t u = (uint32_t)rnd64();
  uint32_t e = 127 - 20 + (u >> 23) % 41;    // 2^-20 .. 2^20
  u = (u & 0x807FFFFFu) | (e << 23);
  float f;
  std::memcpy(&f, &u, 4);
  return (double)f;
}

static bool same(double x, double y) {
  if (x != x && y != y) return true;   // both NaN
  return to_bits(x) == to_bits(y);
}

int main(int argc, char** argv) {
  long n = (argc > 1 ? atol(argv[1]) : 20) * 1000000L;
  if (!_get_FMA3_enable()) { std::printf("this CPU has no FMA3: no reference\n"); return 2; }
  const unsigned csr0 = _mm_getcsr();
  long total = 0, handled = 0, bad = 0, declined_floatvalued = 0;
  const char* rc_name[4] = {"nearest", "down", "up", "zero"};
  for (int flush = 0; flush < 2; flush++) {
    for (int rc = 0; rc < 4; rc++) {
      unsigned csr = (csr0 & ~0xE040u) | ((unsigned)rc << 13) | (flush ? 0x8040u : 0u);
      _mm_setcsr(csr);
      long set_bad = 0, set_handled = 0;
      for (long i = 0; i < n; i++) {
        double a, b, c;
        int kind = (int)(i & 7);
        switch (kind) {
          case 0: a = rnd_float_valued(); b = rnd_float_valued(); c = from_bits(rnd64()); break;       // anything
          case 1: a = rnd_float_moderate(); b = rnd_float_moderate(); c = rnd_float_moderate(); break;   // typical
          case 2: a = rnd_float_moderate(); b = rnd_float_moderate(); c = -(a * b); break;               // exact cancellation
          case 3: a = rnd_float_moderate(); b = rnd_float_moderate(); c = std::nextafter(-(a * b), 1e300); break;  // nearly cancels
          case 4: a = rnd_float_moderate(); b = rnd_float_moderate(); c = ldexp(rnd_float_moderate(), (int)(rnd64() % 140) - 70); break;  // wide exponent gap
          case 5: a = (rnd64() & 1) ? 0.0 : -0.0; b = rnd_float_moderate(); c = (rnd64() & 1) ? 0.0 : -0.0; break;   // signed zeros
          case 6: a = rnd_float_valued(); b = rnd_float_valued(); c = rnd_float_valued(); break;         // all floats incl. denormal floats
          default: a = rnd_float_moderate(); b = rnd_float_moderate(); c = ldexp(a * b, -(int)(rnd64() % 60)); break;  // c near the product's size
        }
        double got;
        if (!mcla_fma_exact(a, b, c, &got)) continue;
        double want = hw_fma(a, b, c);
        set_handled++;
        if (!same(got, want)) {
          if (set_bad < 5) std::printf("MISMATCH rc=%s flush=%d kind=%d a=%a b=%a c=%a got=%a want=%a\n", rc_name[rc], flush, kind, a, b, c, got, want);
          set_bad++;
        }
      }
      std::printf("rounding %-7s FTZ/DAZ %-3s: %ld cases, fast path applied to %ld, mismatches %ld\n", rc_name[rc], flush ? "on" : "off", n, set_handled, set_bad);
      total += n; handled += set_handled; bad += set_bad;
    }
  }
  _mm_setcsr(csr0);
  std::printf("TOTAL %ld cases, applied %ld, MISMATCHES %ld\n", total, handled, bad);

  // speed (this core, FMA3 CPU; the fast path against the hardware call and the library's call)
  {
    static double A[4096], B[4096], C[4096];
    for (int i = 0; i < 4096; i++) { A[i] = rnd_float_moderate(); B[i] = rnd_float_moderate(); C[i] = rnd_float_moderate(); }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    const long it = 50000000L;
    double acc = 0;
    QueryPerformanceCounter(&t0);
    for (long i = 0; i < it; i++) { int k = (int)(i & 4095); double r; mcla_fma_exact(A[k], B[k], C[k], &r); acc += r; }
    QueryPerformanceCounter(&t1);
    std::printf("fast path, independent: %.2f ns/op (acc %g)\n", (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)f.QuadPart / it, acc);
    double x = 0.25;
    QueryPerformanceCounter(&t0);
    for (long i = 0; i < it; i++) { int k = (int)(i & 4095); double r = 0; mcla_fma_exact(A[k], B[k], x * 0.5, &r); x = (double)(float)r; }
    QueryPerformanceCounter(&t1);
    std::printf("fast path, dependent chain: %.2f ns/op (x %g)\n", (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)f.QuadPart / it, x);
  }
  return bad == 0 ? 0 : 1;
}

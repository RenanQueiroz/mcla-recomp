// Microbenchmark of the host operations that the generated code leans on, built like the game (llvm-mingw, UCRT, -msse4.1,
// no FMA): std::fma -> a call to ucrtbase!fma, which runs vfmadd213sd with FMA3 and a long software routine without; and
// ldmxcsr/stmxcsr (the flush-mode switches). It does not run the game. Prints one line per measurement:
//   name  ns/op  cycles/op (at the core clock measured here)  checksum
// usage: bench_fma.exe [iterations-in-millions]
#include <cpuid.h>
#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// Declared in <stdlib.h>/<math.h> of the UCRT; present in libucrt.a (the exports _set_FMA3_enable/_get_FMA3_enable).
extern int _set_FMA3_enable(int);
extern int _get_FMA3_enable(void);

static double g_sink;
static uint64_t g_tsc_hz;

static double now_ns(void) {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1e9 / (double)f.QuadPart;
}

// The core clock: a chain of dependent 1-cycle integer adds.
static double core_ghz(void) {
  volatile uint64_t x = 0;
  uint64_t n = 400000000ull, v = 0;
  double t0 = now_ns();
  for (uint64_t i = 0; i < n; i++) {
    __asm__ volatile("addq $1, %0\n\taddq $1, %0\n\taddq $1, %0\n\taddq $1, %0" : "+r"(v));
  }
  double t1 = now_ns();
  x = v;
  (void)x;
  return (double)n * 4.0 / (t1 - t0);  // adds per ns = GHz
}

static uint32_t rng_state = 12345;
static inline float rnd01(void) {
  rng_state = rng_state * 1664525u + 1013904223u;
  return (float)(rng_state >> 8) * (1.0f / 16777216.0f);
}

#define N 4096
static double A[N], B[N], C[N];

static void fill(int mode) {
  for (int i = 0; i < N; i++) {
    float a = rnd01() * 2.0f - 1.0f, b = rnd01() * 2.0f - 1.0f, c = rnd01() * 2.0f - 1.0f;
    switch (mode) {
      case 0: break;                                   // ordinary audio-like values, all float-valued
      case 1: c = 0.0f; break;                         // accumulator starts at zero
      case 2: a = 0.0f; break;                         // zero gain
      case 3: a *= 1e-20f; b *= 1e-20f; break;         // product far below c
      case 4: c *= 1e-20f; break;                      // c far below the product
      case 5: a *= 1e-20f; b *= 1e-20f; c *= 1e-30f; break;  // float-denormal-ish range products (normal doubles)
    }
    A[i] = a; B[i] = b; C[i] = c;
  }
}

typedef double (*fma_fn)(double, double, double);
static __attribute__((noinline)) double real_fma(double a, double b, double c) { return fma(a, b, c); }
static __attribute__((noinline)) double plain_madd(double a, double b, double c) { return (double)(float)(a * b + c); }

static double run_indep(fma_fn f, long iters) {
  double acc = 0;
  for (long i = 0; i < iters; i++) {
    int k = (int)(i & (N - 1));
    acc += f(A[k], B[k], C[k]);
  }
  return acc;
}
static double run_chain(fma_fn f, long iters) {  // a dependent chain, like f1 = fmadds(f2, f3, f1)
  double acc = 0.25;
  for (long i = 0; i < iters; i++) {
    int k = (int)(i & (N - 1));
    acc = (double)(float)f(A[k], B[k], acc * 0.5);
  }
  return acc;
}

static void report(const char* name, double t0, double t1, long iters, double ghz, double sum) {
  double ns = (t1 - t0) / (double)iters;
  printf("%-44s %9.2f ns/op %8.1f cycles/op   (sum %g)\n", name, ns, ns * ghz, sum);
  g_sink += sum;
}

static void test_fma(const char* tag, long iters, double ghz) {
  const char* modes[] = {"normal", "c=0", "a=0", "prod<<c", "c<<prod", "tiny-prod"};
  char buf[96];
  for (int m = 0; m < 6; m++) {
    fill(m);
    double s;
    double t0 = now_ns();
    s = run_indep(real_fma, iters);
    double t1 = now_ns();
    snprintf(buf, sizeof buf, "fma %s independent, %s", tag, modes[m]);
    report(buf, t0, t1, iters, ghz, s);
    t0 = now_ns();
    s = run_chain(real_fma, iters);
    t1 = now_ns();
    snprintf(buf, sizeof buf, "fma %s dependent chain, %s", tag, modes[m]);
    report(buf, t0, t1, iters, ghz, s);
  }
}

static void test_mxcsr(long iters, double ghz) {
  fill(0);
  unsigned csr0 = _mm_getcsr();
  unsigned flush = csr0 | 0x8040, noflush = csr0 & ~0x8040u;
  double s = 0, t0, t1;
  // (a) no write at all: 4 dependent scalar ops per step, as the baseline
  t0 = now_ns();
  {
    double acc = 0.5;
    for (long i = 0; i < iters; i++) {
      int k = (int)(i & (N - 1));
      acc = (double)(float)(acc * A[k] + B[k]);
      acc = (double)(float)(acc * 0.99 + C[k]);
    }
    s = acc;
  }
  t1 = now_ns();
  report("2 dependent float ops per step (no ldmxcsr)", t0, t1, iters, ghz, s);
  // (b) the same with two ldmxcsr per step (flush on, flush off): what 6,350 writes a round would be 3,175 of
  t0 = now_ns();
  {
    double acc = 0.5;
    for (long i = 0; i < iters; i++) {
      int k = (int)(i & (N - 1));
      _mm_setcsr(flush);
      acc = (double)(float)(acc * A[k] + B[k]);
      _mm_setcsr(noflush);
      acc = (double)(float)(acc * 0.99 + C[k]);
    }
    s = acc;
  }
  t1 = now_ns();
  report("same + 2 ldmxcsr (flush on / off) per step", t0, t1, iters, ghz, s);
  // (c) ldmxcsr back to back, same value (what the unconditional switches do)
  t0 = now_ns();
  for (long i = 0; i < iters; i++) {
    _mm_setcsr(flush);
    _mm_setcsr(noflush);
  }
  t1 = now_ns();
  report("2 ldmxcsr back to back (per pair)", t0, t1, iters, ghz, 0);
  // (d) stmxcsr
  t0 = now_ns();
  unsigned x = 0;
  for (long i = 0; i < iters; i++) x += _mm_getcsr();
  t1 = now_ns();
  report("stmxcsr", t0, t1, iters, ghz, x);
  _mm_setcsr(csr0);
}

// Denormal results of a double->float conversion in the scalar mode (FTZ/DAZ off) and with them on.
static void test_denorm(long iters, double ghz) {
  unsigned csr0 = _mm_getcsr();
  double s = 0, t0, t1;
  volatile double tiny = 1e-40;  // a double that is a float denormal
  volatile double nrm = 1e-3;
  _mm_setcsr(csr0 & ~0x8040u);
  t0 = now_ns();
  for (long i = 0; i < iters; i++) s += (double)(float)(tiny * 0.5);
  t1 = now_ns();
  report("double*0.5 -> float denormal result, FTZ off", t0, t1, iters, ghz, s);
  t0 = now_ns();
  for (long i = 0; i < iters; i++) s += (double)(float)(nrm * 0.5);
  t1 = now_ns();
  report("double*0.5 -> float normal result, FTZ off", t0, t1, iters, ghz, s);
  volatile float ftiny = 1e-40f;
  t0 = now_ns();
  for (long i = 0; i < iters; i++) s += (double)(float)ftiny;
  t1 = now_ns();
  report("float denormal -> double -> float, DAZ off", t0, t1, iters, ghz, s);
  // vector (single precision) multiply with denormal results
  __m128 v = _mm_set1_ps(1e-30f), w = _mm_set1_ps(1e-10f), acc = _mm_setzero_ps();
  t0 = now_ns();
  for (long i = 0; i < iters; i++) acc = _mm_add_ps(acc, _mm_mul_ps(v, w));
  t1 = now_ns();
  report("mulps with denormal result, FTZ off", t0, t1, iters, ghz, _mm_cvtss_f32(acc));
  _mm_setcsr(csr0 | 0x8040);
  acc = _mm_setzero_ps();
  t0 = now_ns();
  for (long i = 0; i < iters; i++) acc = _mm_add_ps(acc, _mm_mul_ps(v, w));
  t1 = now_ns();
  report("mulps with denormal result, FTZ on", t0, t1, iters, ghz, _mm_cvtss_f32(acc));
  _mm_setcsr(csr0);
}

int main(int argc, char** argv) {
  long iters = (argc > 1 ? atol(argv[1]) : 20) * 1000000L;
  SetThreadAffinityMask(GetCurrentThread(), 1ull << 2);  // one fixed core
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  int cpuinfo[4];
  char brand[49] = {0};
  for (int i = 0; i < 3; i++) {
    __cpuid(0x80000002 + i, cpuinfo[0], cpuinfo[1], cpuinfo[2], cpuinfo[3]);
    memcpy(brand + 16 * i, cpuinfo, 16);
  }
  printf("cpu: %s\n", brand);
  __cpuid(1, cpuinfo[0], cpuinfo[1], cpuinfo[2], cpuinfo[3]);
  printf("cpuid1 ecx=%08x (FMA bit12=%d AVX bit28=%d)\n", cpuinfo[2], (cpuinfo[2] >> 12) & 1, (cpuinfo[2] >> 28) & 1);
  int en = -1;
  int has = _get_FMA3_enable(); en = has;
  printf("_get_FMA3_enable: ret=%d value=%d\n", has, en);
  double ghz = core_ghz();
  printf("core clock measured by a dependent add chain: %.3f GHz\n", ghz);
  printf("iterations per test: %ld\n", iters);

  fill(0);
  real_fma(1, 2, 3);
  test_fma("hardware path", iters, ghz);
  // the reference without any libm: a float multiply and add in double (not fused)
  {
    double t0 = now_ns();
    double s = run_chain(plain_madd, iters);
    double t1 = now_ns();
    report("non-fused double(float(a*b+c)), dependent chain", t0, t1, iters, ghz, s);
    t0 = now_ns();
    s = run_indep(plain_madd, iters);
    t1 = now_ns();
    report("non-fused double(float(a*b+c)), independent", t0, t1, iters, ghz, s);
  }
  test_mxcsr(iters, ghz);
  test_denorm(iters, ghz);

  // now the same with the FMA3 path of the C library disabled: what a CPU without FMA3 runs
  int r = _set_FMA3_enable(0);
  en = _get_FMA3_enable();
  printf("_set_FMA3_enable(0) -> %d; now enabled=%d\n", r, en);
  test_fma("SOFTWARE path (FMA3 off)", iters / 4, ghz);
  printf("sink %g\n", g_sink);
  return 0;
}

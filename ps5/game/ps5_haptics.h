// DualSense haptics on PS5.
//
// A DualSense opens in advanced vibration mode (1), where its voice-coil
// actuators play an audio stream and scePadSetVibration's two motor levels do
// nothing. Without a stream the game's rumble is lost, so by default the pad is
// put in compatible mode (2), the classic two-motor emulation.
//
// With mcla_pad_haptics on, the pad stays in advanced mode and this feeds the
// controller's vibration audio port (sceAudioOutOpen type 10) instead: 256
// stereo samples at 48 kHz per output call, left and right actuator. The signal
// is the game's own low end (its LFE channel and some of the front channels,
// low-passed, so engine, impacts and nitro are felt in time with the sound)
// plus the game's XInput rumble levels made into a texture (large motor: low
// and left-heavy; small motor: higher and right-heavy). If the port does not
// open, or stops taking buffers, the pad goes back to compatible mode.
//
// The same port and mode values as mihawk-99/PS5_RetroArch (src/input_ps5.cpp).

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <unistd.h>

#include <rex/cvar.h>

#include "title_log.h"

REXCVAR_DEFINE_BOOL(mcla_pad_haptics, false, "MCLA",
                    "DualSense haptics from the game's sound and rumble; off: two-motor rumble");
REXCVAR_DEFINE_DOUBLE(mcla_haptics_gain, 1.0, "MCLA", "Overall haptics strength").range(0.0, 4.0);
REXCVAR_DEFINE_DOUBLE(mcla_haptics_audio, 1.5, "MCLA",
                      "Strength of the part taken from the game's sound").range(0.0, 8.0);
REXCVAR_DEFINE_DOUBLE(mcla_haptics_rumble, 0.6, "MCLA",
                      "Strength of the part made from the game's rumble").range(0.0, 4.0);
REXCVAR_DEFINE_DOUBLE(mcla_haptics_lfe_mix, 1.0, "MCLA",
                      "Share of the LFE channel in the sound part").range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(mcla_haptics_front_mix, 0.3, "MCLA",
                      "Share of the front channels in the sound part").range(0.0, 2.0);
REXCVAR_DEFINE_DOUBLE(mcla_haptics_cutoff_hz, 140.0, "MCLA",
                      "Low-pass cut-off of the sound part, Hz").range(30.0, 400.0);

extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, uint32_t length, uint32_t frequency,
                    uint32_t format);
int sceAudioOutOutput(int handle, const void* data);
int sceAudioOutClose(int handle);
int scePadSetVibration(int handle, const void* param);
int scePadSetVibrationMode(int handle, int mode);
}

inline void MclaHapticsLog(const char* format, ...) {
  char text[256];
  va_list args;
  va_start(args, format);
  int length = std::vsnprintf(text, sizeof text - 1, format, args);
  va_end(args);
  length = std::clamp(length, 0, static_cast<int>(sizeof text) - 2);
  text[length] = '\n';
  (void)!write(g_mcla_log_fd, text, static_cast<size_t>(length) + 1);
}

class Ps5Haptics {
 public:
  ~Ps5Haptics() { Stop(); }

  // Called once the pad is open. Chooses advanced mode with the stream, or
  // compatible mode.
  void Start(int pad_handle, int user_id) {
    pad_handle_ = pad_handle;
    if (REXCVAR_GET(mcla_pad_haptics) && OpenPort(user_id)) {
      const int mode = scePadSetVibrationMode(pad_handle_, kModeAdvanced);
      MclaHapticsLog("PS5 haptics: vibration port 0x%08X (%s), advanced mode 0x%08X", port_,
                     float_format_ ? "f32" : "s16", static_cast<uint32_t>(mode));
      if (mode == 0) {
        active_ = true;
        running_ = true;
        worker_ = std::thread([this] { Run(); });
        return;
      }
      sceAudioOutClose(port_);
      port_ = -1;
    }
    const int mode = scePadSetVibrationMode(pad_handle_, kModeCompatible);
    MclaHapticsLog("PS5 haptics: %s; compatible (two-motor) mode 0x%08X",
                   REXCVAR_GET(mcla_pad_haptics) ? "vibration port unavailable" : "off",
                   static_cast<uint32_t>(mode));
  }

  void Stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    if (port_ >= 0) {
      sceAudioOutClose(port_);
      port_ = -1;
    }
    if (pad_handle_ >= 0) {
      const uint8_t off[2] = {0, 0};
      scePadSetVibration(pad_handle_, off);
    }
  }

  // The game's latest rumble levels, 0..255.
  uint8_t large() const { return large_.load(std::memory_order_relaxed); }
  uint8_t small() const { return small_.load(std::memory_order_relaxed); }

  // The game's rumble, 0..255 per motor.
  void SetRumble(uint8_t large, uint8_t small) {
    large_.store(large, std::memory_order_relaxed);
    small_.store(small, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ && pad_handle_ >= 0) {
      const uint8_t param[2] = {large, small};
      scePadSetVibration(pad_handle_, param);
    }
  }

  // One guest audio frame: 6 channels x 256 samples, channel after channel,
  // big-endian floats (FL, FR, FC, LFE, BL, BR). Called from the audio thread.
  void PushAudio(const float* frame) {
    if (!active_) return;
    const float lfe_mix = static_cast<float>(REXCVAR_GET(mcla_haptics_lfe_mix));
    const float front_mix = static_cast<float>(REXCVAR_GET(mcla_haptics_front_mix)) / 3.0f;
    Block block;
    for (uint32_t i = 0; i < kSamples; ++i) {
      const float fl = Swap(frame[0 * kSamples + i]);
      const float fr = Swap(frame[1 * kSamples + i]);
      const float fc = Swap(frame[2 * kSamples + i]);
      const float lfe = Swap(frame[3 * kSamples + i]);
      block.samples[i] = lfe * lfe_mix + (fl + fr + fc) * front_mix;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // Keep the delay short: when the haptics thread falls behind, drop the
    // oldest block rather than let the feel lag the sound.
    if (queued_ == kQueue) {
      read_ = (read_ + 1) % kQueue;
      --queued_;
      ++dropped_;
    }
    queue_[(read_ + queued_) % kQueue] = block;
    ++queued_;
  }

 private:
  static constexpr int kModeAdvanced = 1;
  static constexpr int kModeCompatible = 2;
  static constexpr int kPortVibration = 10;
  static constexpr uint32_t kSamples = 256;
  static constexpr uint32_t kRate = 48000;
  static constexpr uint32_t kFormatS16Stereo = 1;
  static constexpr uint32_t kFormatF32Stereo = 4;
  static constexpr int kFailuresToGiveUp = 40;
  static constexpr int kQueue = 4;

  struct Block {
    float samples[kSamples];
  };

  static float Swap(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    bits = __builtin_bswap32(bits);
    std::memcpy(&value, &bits, sizeof bits);
    return std::isfinite(value) ? value : 0.0f;
  }

  // A soft limit: linear when quiet, approaching +-1 smoothly.
  static float SoftLimit(float x) {
    x = std::clamp(x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
  }

  bool OpenPort(int user_id) {
    const int init = sceAudioOutInit();
    port_ = sceAudioOutOpen(user_id, kPortVibration, 0, kSamples, kRate, kFormatS16Stereo);
    const int s16_result = port_;
    if (port_ < 0) {
      float_format_ = true;
      port_ = sceAudioOutOpen(user_id, kPortVibration, 0, kSamples, kRate, kFormatF32Stereo);
    }
    if (port_ < 0) {
      MclaHapticsLog("PS5 haptics: sceAudioOutInit 0x%08X; vibration port s16 0x%08X, f32 0x%08X",
                     static_cast<uint32_t>(init), static_cast<uint32_t>(s16_result),
                     static_cast<uint32_t>(port_));
      return false;
    }
    return true;
  }

  void FallBack() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    const int mode = scePadSetVibrationMode(pad_handle_, kModeCompatible);
    const uint8_t param[2] = {large_.load(), small_.load()};
    scePadSetVibration(pad_handle_, param);
    MclaHapticsLog("PS5 haptics: vibration port stopped taking buffers; compatible mode 0x%08X",
                   static_cast<uint32_t>(mode));
  }

  void Run() {
    constexpr float kTwoPi = 6.2831853f;
    alignas(16) int16_t out_s16[kSamples * 2];
    alignas(16) float out_f32[kSamples * 2];
    float lp1 = 0.0f, lp2 = 0.0f, dc = 0.0f;
    float env_large = 0.0f, env_small = 0.0f, noise_l = 0.0f, noise_r = 0.0f;
    float phase_low = 0.0f, phase_high = 0.0f;
    uint32_t rng = 0x9E3779B9u;
    int failures = 0;
    float peak_in = 0.0f, peak_out = 0.0f;
    uint32_t blocks = 0, starved = 0;
    while (running_) {
      Block block;
      bool have_audio = false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queued_ > 0) {
          block = queue_[read_];
          read_ = (read_ + 1) % kQueue;
          --queued_;
          have_audio = true;
        }
      }
      if (!have_audio) ++starved;
      const float gain = static_cast<float>(REXCVAR_GET(mcla_haptics_gain));
      const float audio_gain = static_cast<float>(REXCVAR_GET(mcla_haptics_audio));
      const float rumble_gain = static_cast<float>(REXCVAR_GET(mcla_haptics_rumble));
      const float cutoff = static_cast<float>(REXCVAR_GET(mcla_haptics_cutoff_hz));
      const float a = 1.0f - std::exp(-kTwoPi * cutoff / kRate);
      const float target_large = large_.load(std::memory_order_relaxed) / 255.0f;
      const float target_small = small_.load(std::memory_order_relaxed) / 255.0f;
      for (uint32_t i = 0; i < kSamples; ++i) {
        // The sound part: two one-pole low-passes, then the DC taken out so
        // the actuators are not pushed to one side.
        const float in = have_audio ? block.samples[i] : 0.0f;
        peak_in = std::max(peak_in, std::fabs(in));
        lp1 += a * (in - lp1);
        lp2 += a * (lp1 - lp2);
        dc += 0.0005f * (lp2 - dc);
        const float sound = (lp2 - dc) * audio_gain;

        // The rumble part, eased so a level change does not click.
        env_large += (target_large - env_large) * 0.004f;
        env_small += (target_small - env_small) * 0.004f;
        rng = rng * 1664525u + 1013904223u;
        noise_l += 0.08f * ((static_cast<float>((rng >> 9) & 0xFFFF) / 32768.0f - 1.0f) - noise_l);
        rng = rng * 1664525u + 1013904223u;
        noise_r += 0.08f * ((static_cast<float>((rng >> 9) & 0xFFFF) / 32768.0f - 1.0f) - noise_r);
        phase_low += 55.0f / kRate;
        phase_high += 190.0f / kRate;
        if (phase_low >= 1.0f) phase_low -= 1.0f;
        if (phase_high >= 1.0f) phase_high -= 1.0f;
        const float low = 0.7f * std::sin(kTwoPi * phase_low);
        const float high = 0.55f * std::sin(kTwoPi * phase_high);
        const float rumble_l = env_large * (low + 0.5f * noise_l) + 0.5f * env_small * (high + 0.45f * noise_r);
        const float rumble_r = 0.5f * env_large * (low + 0.5f * noise_r) + env_small * (high + 0.45f * noise_l);

        const float left = SoftLimit((sound + rumble_l * rumble_gain) * gain);
        const float right = SoftLimit((sound + rumble_r * rumble_gain) * gain);
        peak_out = std::max(peak_out, std::max(std::fabs(left), std::fabs(right)));
        out_f32[2 * i] = left;
        out_f32[2 * i + 1] = right;
        out_s16[2 * i] = static_cast<int16_t>(left * 32000.0f);
        out_s16[2 * i + 1] = static_cast<int16_t>(right * 32000.0f);
      }
      const int result = sceAudioOutOutput(port_, float_format_ ? static_cast<const void*>(out_f32)
                                                                : static_cast<const void*>(out_s16));
      if (result < 0) {
        if (failures == 0) {
          MclaHapticsLog("PS5 haptics: sceAudioOutOutput failed: 0x%08X", static_cast<uint32_t>(result));
        }
        if (++failures >= kFailuresToGiveUp) {
          FallBack();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      failures = 0;
      // About every 10 s: how much signal there was, to tune the gains by.
      if (++blocks % 1875 == 0) {
        uint32_t dropped;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          dropped = dropped_;
        }
        MclaHapticsLog("PS5 haptics: 10 s peak sound in %.3f, out %.3f; blocks without sound %u, "
                       "dropped %u",
                       peak_in, peak_out, starved, dropped);
        peak_in = peak_out = 0.0f;
        starved = 0;
      }
    }
  }

  int pad_handle_ = -1;
  int port_ = -1;
  bool float_format_ = false;
  std::atomic<bool> active_{false};
  std::atomic<bool> running_{false};
  std::atomic<uint8_t> large_{0};
  std::atomic<uint8_t> small_{0};
  std::mutex mutex_;
  Block queue_[kQueue];
  int read_ = 0;
  int queued_ = 0;
  uint32_t dropped_ = 0;
  std::thread worker_;
};

inline Ps5Haptics g_mcla_haptics;

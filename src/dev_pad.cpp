// Scripted controller input for unattended runs (fork-only development tool;
// see launcher/README.md). Off unless one of the mcla_pad_* options is set.
//
// A synthetic input device ("ScriptPad") is added to the SDK's input system
// next to the real pads. Synthetic devices always feed guest user 0 and merge
// with whatever else feeds it, so the real controller keeps working. Commands
// come from a script file (mcla_pad_script) and/or a named pipe
// (mcla_pad_pipe, driven by launcher/dev/pad.py), one per line:
//
//   press a [150ms]   hold rt 2s   down x   up x   release [all|name]
//   trigger rt 0.8 [2s]   stick lx -0.5 [1s]
//   wait 2s | 500ms | 90f (engine frames)
//   wait_until <condition> [timeout]
//   press_until <button> <condition> [every=2s] [hold=300ms] [timeout=120s]
//   pos   peek <addr> [bytes]   peeku <addr> [count]   peekf <addr> [count]
//   dump <addr> <bytes> <file>   mark <name>   shot <file.png> [scale]
//   state   log <text>   include <script>   quit
//   play <recording.csv> [from=<mark>]   record start <file.csv> | record stop
//
// Conditions: left_title (Start accepted on the title screen), loading (the
// map zoom-out), roam (the player is in the city), frames N (engine frames
// since start-up), u32|f32 <addr> ==|!=|<|>|<=|>= <value>. Addresses are guest
// addresses, with [x] for "the 32-bit value at x" and +/- offsets:
// [0x8288DCF8]+0x2A0.
//
// The game ignores buttons during its own transitions (the title screen's
// camera cuts, a menu closing), so scripts press until a condition holds.
//
// Engine frames are counted in mcla_frame_delta, the game's per-update timer,
// which the start-up logos never reach. mcla_fixed_dt=N makes every update
// advance exactly 1/N s: uncapped, that is a timedemo (the same updates, drawn
// as fast as the PC can), for A/B measurements only.

#include "generated/default/mcla_init.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_STRING(mcla_pad_pipe, "", "MCLA",
                      "Development: serve scripted controller commands on \\\\.\\pipe\\<name>");
REXCVAR_DEFINE_STRING(mcla_pad_script, "", "MCLA",
                      "Development: run this controller script from start-up");
REXCVAR_DEFINE_STRING(mcla_pad_record, "", "MCLA",
                      "Development: record what guest user 0's controller does to this CSV");
REXCVAR_DEFINE_STRING(mcla_pad_frames_csv, "", "MCLA",
                      "Development: log the time of every swap, with marks, to this CSV");
REXCVAR_DEFINE_BOOL(mcla_pad_exit_on_end, false, "MCLA",
                    "Development: quit the game when the controller script ends");
REXCVAR_DEFINE_INT32(mcla_fixed_dt, 0, "MCLA",
                     "Development: every game update advances exactly 1/N s (0 = measured "
                     "time). Uncapped this is a timedemo; for A/B measurements only")
    .range(0, 1000);

namespace {

using namespace rex;
using namespace rex::input;
using Clock = std::chrono::steady_clock;

bool g_enabled = false;
rex::ui::Window* g_window = nullptr;

// --- Engine frames and swaps ---

std::atomic<uint64_t> g_frame{0};
std::atomic<uint64_t> g_swaps{0};
std::atomic<uint64_t> g_foreign_frame_calls{0};
std::mutex g_frame_mutex;
std::condition_variable g_frame_cv;

// --- Pad state ---

struct PadValues {
  uint16_t buttons = 0;
  uint8_t lt = 0;
  uint8_t rt = 0;
  int16_t lx = 0;
  int16_t ly = 0;
  int16_t rx = 0;
  int16_t ry = 0;
  bool operator==(const PadValues&) const = default;
};

std::mutex g_pad_mutex;
PadValues g_pad;
std::atomic<uint64_t> g_state_polls{0};
// The thread the game polls the pad on (the last XamInputGetState caller).
std::atomic<size_t> g_poll_thread{0};

size_t ThisThread() {
  return std::hash<std::thread::id>{}(std::this_thread::get_id());
}
// Set while the recorder itself reads the input system.
thread_local bool g_in_record_poll = false;
std::atomic<uint64_t> g_key_polls{0};
std::atomic<uint64_t> g_keys_sent{0};

void SetPad(const PadValues& values) {
  std::lock_guard lock(g_pad_mutex);
  if (!(values == g_pad)) {
    g_pad = values;
  }
}

template <typename F>
void EditPad(F&& edit) {
  std::lock_guard lock(g_pad_mutex);
  PadValues values = g_pad;
  edit(values);
  if (!(values == g_pad)) {
    g_pad = values;
  }
}

PadValues GetPad() {
  std::lock_guard lock(g_pad_mutex);
  return g_pad;
}

// --- Frame log (one line per swap, plus marks) ---

std::mutex g_log_mutex;
FILE* g_frames_csv = nullptr;
Clock::time_point g_start = Clock::now();
Clock::time_point g_last_swap{};

void WriteMark(const std::string& name) {
  std::lock_guard lock(g_log_mutex);
  if (g_frames_csv) {
    std::fprintf(g_frames_csv, "mark,%s,%llu,%llu\n", name.c_str(),
                 static_cast<unsigned long long>(g_swaps.load()),
                 static_cast<unsigned long long>(g_frame.load()));
    std::fflush(g_frames_csv);
  }
}

void CloseFrameLog() {
  std::lock_guard lock(g_log_mutex);
  if (g_frames_csv) {
    std::fclose(g_frames_csv);
    g_frames_csv = nullptr;
  }
}

// --- Recording and playback ---

bool PlayerPosition(float xyz[3]);

struct Row {
  uint64_t frame;
  PadValues values;
};

struct Recording {
  std::vector<Row> rows;
  std::vector<std::pair<uint64_t, std::string>> marks;
  std::vector<std::pair<uint64_t, std::array<float, 3>>> positions;
  uint64_t end = 0;
};

std::mutex g_record_mutex;
FILE* g_record_file = nullptr;
uint64_t g_record_start = 0;
PadValues g_record_last;
bool g_record_first = true;
uint64_t g_record_pos_base = 0;  // positions are logged every 30 frames from the last mark

std::mutex g_play_mutex;
std::shared_ptr<Recording> g_play;
uint64_t g_play_start = 0;  // engine frame the playback started on
uint64_t g_play_base = 0;   // recording frame that start maps to
size_t g_play_index = 0;
size_t g_play_pos_index = 0;
double g_play_max_dev = 0;  // metres from the recorded positions
int g_play_pos_checked = 0;
std::atomic<bool> g_playing{false};

void WriteRecordRow(uint64_t frame, const PadValues& v) {
  std::fprintf(g_record_file, "%llu,%04X,%u,%u,%d,%d,%d,%d\n",
               static_cast<unsigned long long>(frame), v.buttons, v.lt, v.rt, v.lx, v.ly, v.rx,
               v.ry);
}

bool StartRecording(const std::string& path, std::string& error) {
  std::lock_guard lock(g_record_mutex);
  if (g_record_file) {
    error = "already recording";
    return false;
  }
  g_record_file = std::fopen(path.c_str(), "w");
  if (!g_record_file) {
    error = "cannot write " + path;
    return false;
  }
  std::fprintf(g_record_file, "# mcla-pad 1 fixed_dt=%d\nframe,buttons,lt,rt,lx,ly,rx,ry\n",
               REXCVAR_GET(mcla_fixed_dt));
  g_record_start = g_frame.load();
  g_record_first = true;
  REXLOG_INFO("dev pad: recording to {} from engine frame {}", path, g_record_start);
  return true;
}

void StopRecording() {
  std::lock_guard lock(g_record_mutex);
  if (g_record_file) {
    std::fprintf(g_record_file, "end,%llu\n",
                 static_cast<unsigned long long>(g_frame.load() - g_record_start));
    std::fclose(g_record_file);
    g_record_file = nullptr;
  }
}

void RecordMark(const std::string& name) {
  std::lock_guard lock(g_record_mutex);
  if (g_record_file) {
    g_record_pos_base = g_frame.load() - g_record_start;
    std::fprintf(g_record_file, "mark,%llu,%s\n",
                 static_cast<unsigned long long>(g_record_pos_base), name.c_str());
  }
}

// What the game reads for user 0: the real pad, keyboard emulation and this
// device merged. Called on the game's update thread, which is also the thread
// that polls input, so the input system is not used from two threads at once.
void RecordFrame(uint64_t frame) {
  std::lock_guard lock(g_record_mutex);
  if (!g_record_file) {
    return;
  }
  // The input system has no lock of its own. MCLA polls it on this thread
  // (checked at every frame here); should a poll ever come from another one,
  // skip the frame rather than read the device lists while they change.
  if (const size_t poller = g_poll_thread.load(); poller && poller != ThisThread()) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      REXLOG_WARN("dev pad: the game polls input on another thread; recording skips frames");
    }
    return;
  }
  auto* input = static_cast<rex::input::InputSystem*>(
      rex::Runtime::instance() ? rex::Runtime::instance()->input_system() : nullptr);
  if (!input) {
    return;
  }
  X_INPUT_STATE state = {};
  g_in_record_poll = true;
  const X_RESULT polled = input->GetState(0, &state);
  g_in_record_poll = false;
  if (polled != X_ERROR_SUCCESS) {
    return;
  }
  PadValues v;
  v.buttons = state.gamepad.buttons;
  v.lt = state.gamepad.left_trigger;
  v.rt = state.gamepad.right_trigger;
  v.lx = state.gamepad.thumb_lx;
  v.ly = state.gamepad.thumb_ly;
  v.rx = state.gamepad.thumb_rx;
  v.ry = state.gamepad.thumb_ry;
  if (g_record_first || !(v == g_record_last)) {
    WriteRecordRow(frame - g_record_start, v);
    g_record_last = v;
    g_record_first = false;
  }
  // Where the car is twice a second at 60, so a playback can tell how far it
  // went off the recorded route.
  float xyz[3];
  if ((frame - g_record_start - g_record_pos_base) % 30 == 0 && PlayerPosition(xyz)) {
    std::fprintf(g_record_file, "pos,%llu,%.3f,%.3f,%.3f\n",
                 static_cast<unsigned long long>(frame - g_record_start), xyz[0], xyz[1], xyz[2]);
  }
}

std::shared_ptr<Recording> LoadRecording(const std::string& path, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot read " + path;
    return nullptr;
  }
  auto rec = std::make_shared<Recording>();
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line.rfind("frame,", 0) == 0) {
      continue;
    }
    for (char& c : line) {
      if (c == ',') {
        c = ' ';
      }
    }
    std::istringstream fields(line);
    std::string first;
    fields >> first;
    if (first == "mark") {
      uint64_t frame;
      std::string name;
      fields >> frame >> name;
      rec->marks.emplace_back(frame, name);
    } else if (first == "end") {
      fields >> rec->end;
    } else if (first == "pos") {
      uint64_t frame;
      std::array<float, 3> xyz;
      fields >> frame >> xyz[0] >> xyz[1] >> xyz[2];
      if (!fields.fail()) {
        rec->positions.emplace_back(frame, xyz);
      }
    } else {
      Row row{};
      std::string buttons;
      int lt, rt, lx, ly, rx, ry;
      row.frame = std::stoull(first);
      fields >> buttons >> lt >> rt >> lx >> ly >> rx >> ry;
      if (fields.fail()) {
        error = "bad line: " + line;
        return nullptr;
      }
      row.values.buttons = static_cast<uint16_t>(std::stoul(buttons, nullptr, 16));
      row.values.lt = static_cast<uint8_t>(lt);
      row.values.rt = static_cast<uint8_t>(rt);
      row.values.lx = static_cast<int16_t>(lx);
      row.values.ly = static_cast<int16_t>(ly);
      row.values.rx = static_cast<int16_t>(rx);
      row.values.ry = static_cast<int16_t>(ry);
      rec->rows.push_back(row);
    }
  }
  if (rec->rows.empty()) {
    error = "no rows in " + path;
    return nullptr;
  }
  if (rec->end == 0) {
    rec->end = rec->rows.back().frame + 1;
  }
  return rec;
}

void PlayFrame(uint64_t frame) {
  std::lock_guard lock(g_play_mutex);
  if (!g_play) {
    return;
  }
  const uint64_t at = g_play_base + (frame - g_play_start);
  if (at >= g_play->end) {
    SetPad({});
    g_play.reset();
    g_playing = false;
    return;
  }
  const auto& rows = g_play->rows;
  while (g_play_index + 1 < rows.size() && rows[g_play_index + 1].frame <= at) {
    g_play_index++;
  }
  if (rows[g_play_index].frame <= at) {
    SetPad(rows[g_play_index].values);
  }
  const auto& positions = g_play->positions;
  while (g_play_pos_index < positions.size() && positions[g_play_pos_index].first < at) {
    g_play_pos_index++;
  }
  float xyz[3];
  if (g_play_pos_index < positions.size() && positions[g_play_pos_index].first == at &&
      PlayerPosition(xyz)) {
    const auto& want = positions[g_play_pos_index].second;
    const double dx = xyz[0] - want[0], dy = xyz[1] - want[1], dz = xyz[2] - want[2];
    g_play_max_dev = std::max(g_play_max_dev, std::sqrt(dx * dx + dy * dy + dz * dz));
    g_play_pos_checked++;
  }
}

// --- Guest memory ---

uint8_t* GuestPointer(uint32_t address, size_t length) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime) {
    return nullptr;
  }
  uint8_t* p = runtime->virtual_membase() + address;
#if defined(_WIN32)
  // Unmapped guest pages are reserved, not committed; reading one would fault.
  uint8_t* cursor = p;
  while (cursor < p + length) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(cursor, &info, sizeof info) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      return nullptr;
    }
    cursor = static_cast<uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
#endif
  return p;
}

bool ReadU32(uint32_t address, uint32_t& value) {
  const uint8_t* p = GuestPointer(address, 4);
  if (!p) {
    return false;
  }
  value = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  return true;
}

bool ParseNumber(const std::string& text, double& value) {
  try {
    size_t used = 0;
    if (text.size() > 2 && (text.rfind("0x", 0) == 0 || text.rfind("-0x", 0) == 0)) {
      const bool negative = text[0] == '-';
      value = static_cast<double>(std::stoull(text.substr(negative ? 3 : 2), &used, 16));
      if (negative) {
        value = -value;
      }
      return used == text.size() - (negative ? 3 : 2);
    }
    value = std::stod(text, &used);
    return used == text.size();
  } catch (...) {
    return false;
  }
}

// [x] reads the 32-bit value at x; terms add up: [0x8286D8D4]+0x51240+288.
bool EvalAddress(const std::string& text, size_t& pos, uint32_t& out) {
  uint64_t total = 0;
  int sign = 1;
  bool any = false;
  while (pos < text.size()) {
    uint32_t term = 0;
    if (text[pos] == '[') {
      pos++;
      uint32_t inner;
      if (!EvalAddress(text, pos, inner) || pos >= text.size() || text[pos] != ']') {
        return false;
      }
      pos++;
      if (!ReadU32(inner, term)) {
        return false;
      }
    } else {
      size_t end = pos;
      while (end < text.size() && std::isalnum(static_cast<unsigned char>(text[end]))) {
        end++;
      }
      double number;
      if (end == pos || !ParseNumber(text.substr(pos, end - pos), number)) {
        return false;
      }
      term = static_cast<uint32_t>(static_cast<int64_t>(number));
      pos = end;
    }
    total += sign > 0 ? term : -static_cast<int64_t>(term);
    any = true;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
      sign = text[pos] == '+' ? 1 : -1;
      pos++;
      continue;
    }
    break;
  }
  out = static_cast<uint32_t>(total);
  return any;
}

bool EvalAddress(const std::string& text, uint32_t& out) {
  size_t pos = 0;
  return EvalAddress(text, pos, out) && pos == text.size();
}

// --- Screenshots ---

uint32_t Crc32(const uint8_t* data, size_t length, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t n = 0; n < 256; n++) {
      uint32_t c = n;
      for (int k = 0; k < 8; k++) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      table[n] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < length; i++) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

void PutBE32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void PngChunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
  PutBE32(png, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> body(type, type + 4);
  body.insert(body.end(), data.begin(), data.end());
  png.insert(png.end(), body.begin(), body.end());
  PutBE32(png, Crc32(body.data(), body.size()));
}

// An uncompressed (stored-block) PNG: no zlib needed, and size is no concern
// for a development screenshot.
bool WritePng(const std::string& path, uint32_t width, uint32_t height,
              const std::vector<uint8_t>& rgb) {
  std::vector<uint8_t> raw;
  raw.reserve((width * 3 + 1) * height);
  for (uint32_t y = 0; y < height; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + size_t(y) * width * 3,
               rgb.begin() + size_t(y + 1) * width * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t offset = 0; offset < raw.size() || offset == 0;) {
    const size_t chunk = std::min<size_t>(65535, raw.size() - offset);
    const bool last = offset + chunk >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(chunk));
    z.push_back(uint8_t(chunk >> 8));
    z.push_back(uint8_t(~chunk));
    z.push_back(uint8_t(~chunk >> 8));
    z.insert(z.end(), raw.begin() + offset, raw.begin() + offset + chunk);
    offset += chunk;
    if (last) {
      break;
    }
  }
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  PutBE32(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> header;
  PutBE32(header, width);
  PutBE32(header, height);
  header.insert(header.end(), {8, 2, 0, 0, 0});
  PngChunk(png, "IHDR", header);
  PngChunk(png, "IDAT", z);
  PngChunk(png, "IEND", {});
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
  std::fclose(f);
  return ok;
}

bool Shot(const std::string& path, double scale, std::string& result) {
  auto* runtime = rex::Runtime::instance();
  auto* graphics = runtime ? runtime->graphics_system() : nullptr;
  rex::ui::Presenter* presenter = graphics ? graphics->presenter() : nullptr;
  if (!presenter) {
    result = "no presenter";
    return false;
  }
  rex::ui::RawImage image;
  if (!presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
    result = "no guest output yet";
    return false;
  }
  scale = std::clamp(scale, 0.05, 1.0);
  const uint32_t width = std::max<uint32_t>(1, uint32_t(image.width * scale));
  const uint32_t height = std::max<uint32_t>(1, uint32_t(image.height * scale));
  std::vector<uint8_t> rgb(size_t(width) * height * 3);
  for (uint32_t y = 0; y < height; y++) {
    const uint32_t sy = std::min(image.height - 1, uint32_t((y + 0.5) / scale));
    for (uint32_t x = 0; x < width; x++) {
      const uint32_t sx = std::min(image.width - 1, uint32_t((x + 0.5) / scale));
      const uint8_t* src = image.data.data() + sy * image.stride + sx * 4;
      uint8_t* dst = rgb.data() + (size_t(y) * width + x) * 3;
      dst[0] = src[0];
      dst[1] = src[1];
      dst[2] = src[2];
    }
  }
  if (!WritePng(path, width, height, rgb)) {
    result = "cannot write " + path;
    return false;
  }
  result = std::to_string(width) + "x" + std::to_string(height);
  return true;
}

// --- Game facts ---

// The vehicle the player drives (LARecomp's carbon_parts.cpp; checked live:
// zero on the title screen, set once the player's car exists).
constexpr uint32_t kPlayerVehicle = 0x8288DCF8;
// Its position, three floats (x, height, z) of its matrix's translation row.
constexpr uint32_t kVehiclePositionOffset = 0x2A0;
// Found by diffing guest memory across runs on the title screen, during the
// map zoom-out and in the city, and checked in later runs:
// - 0 until Start is accepted on the title screen, then 1;
constexpr uint32_t kLeftTitle = 0x82836A28;
// - 3 from just after that until the player is in the city, then 0 (on the
//   title screen it is 0 or 2, depending on the run);
constexpr uint32_t kGameState = 0x8288D000;
// - 4 until the city is ready to play, then 0.
constexpr uint32_t kCityPending = 0x8286CF54;

uint32_t ReadWord(uint32_t address) {
  uint32_t value = 0xFFFFFFFF;
  ReadU32(address, value);
  return value;
}

uint32_t GameState() {
  return ReadWord(kGameState);
}

bool PlayerPosition(float xyz[3]) {
  uint32_t vehicle = 0;
  if (!ReadU32(kPlayerVehicle, vehicle) || !vehicle) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    uint32_t raw;
    if (!ReadU32(vehicle + kVehiclePositionOffset + 4 * i, raw)) {
      return false;
    }
    std::memcpy(&xyz[i], &raw, 4);
  }
  return true;
}

// --- Commands ---

struct ButtonName {
  const char* name;
  uint16_t bit;
};
constexpr ButtonName kButtons[] = {
    {"up", 0x0001},    {"down", 0x0002}, {"left", 0x0004},   {"right", 0x0008},
    {"start", 0x0010}, {"back", 0x0020}, {"ls", 0x0040},     {"rs", 0x0080},
    {"lb", 0x0100},    {"rb", 0x0200},   {"a", 0x1000},      {"b", 0x2000},
    {"x", 0x4000},     {"y", 0x8000},
};

uint16_t ButtonBit(const std::string& name) {
  for (const auto& b : kButtons) {
    if (name == b.name) {
      return b.bit;
    }
  }
  return 0;
}

// "150ms", "2s", "1.5s" -> milliseconds; "90f" -> engine frames.
bool ParseDuration(const std::string& text, double& amount, bool& frames) {
  frames = false;
  std::string number = text;
  double unit = 1.0;
  if (text.size() > 2 && text.ends_with("ms")) {
    number = text.substr(0, text.size() - 2);
  } else if (text.size() > 1 && text.back() == 's') {
    number = text.substr(0, text.size() - 1);
    unit = 1000.0;
  } else if (text.size() > 1 && text.back() == 'f') {
    number = text.substr(0, text.size() - 1);
    frames = true;
  } else {
    return false;
  }
  if (!ParseNumber(number, amount) || amount < 0) {
    return false;
  }
  amount *= unit;
  return true;
}

void WaitFrames(uint64_t count) {
  std::unique_lock lock(g_frame_mutex);
  const uint64_t target = g_frame.load() + count;
  g_frame_cv.wait(lock, [&] { return g_frame.load() >= target; });
}

void Wait(double amount, bool frames) {
  if (frames) {
    WaitFrames(static_cast<uint64_t>(amount));
  } else {
    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(amount * 1000)));
  }
}

bool Compare(double a, const std::string& op, double b, bool& ok) {
  ok = true;
  if (op == "==") return a == b;
  if (op == "!=") return a != b;
  if (op == "<") return a < b;
  if (op == ">") return a > b;
  if (op == "<=") return a <= b;
  if (op == ">=") return a >= b;
  ok = false;
  return false;
}

struct Condition {
  enum Kind { kFrames, kMemory, kLeftTitle, kLoading, kRoam } kind = kFrames;
  bool is_float = false;
  std::string address;
  std::string op;
  double value = 0;
};

bool ParseCondition(const std::vector<std::string>& args, size_t& i, Condition& c,
                    std::string& error) {
  const std::string word = i < args.size() ? args[i] : "";
  if (word == "left_title" || word == "loading" || word == "roam") {
    c.kind = word == "left_title" ? Condition::kLeftTitle
             : word == "loading"  ? Condition::kLoading
                                  : Condition::kRoam;
    i++;
    return true;
  }
  if (word == "frames") {
    c.kind = Condition::kFrames;
    if (i + 1 >= args.size() || !ParseNumber(args[i + 1], c.value)) {
      error = "frames <count>";
      return false;
    }
    i += 2;
    return true;
  }
  if (word == "u32" || word == "f32") {
    c.kind = Condition::kMemory;
    c.is_float = word == "f32";
    bool op_ok = false;
    if (i + 3 < args.size()) {
      Compare(0, args[i + 2], 0, op_ok);
    }
    uint32_t address;
    if (!op_ok || !EvalAddress(args[i + 1], address) || !ParseNumber(args[i + 3], c.value)) {
      error = word + " <addr> <op> <value>";
      return false;
    }
    c.address = args[i + 1];
    c.op = args[i + 2];
    i += 4;
    return true;
  }
  error = "condition: left_title, loading, roam, frames N, u32|f32 <addr> <op> <value>";
  return false;
}

bool Evaluate(const Condition& c) {
  switch (c.kind) {
    case Condition::kFrames:
      return g_frame.load() >= static_cast<uint64_t>(c.value);
    case Condition::kLeftTitle:
      return ReadWord(kLeftTitle) == 1;
    case Condition::kLoading:
      return GameState() == 3;
    case Condition::kRoam: {
      float xyz[3];
      return ReadWord(kLeftTitle) == 1 && GameState() == 0 && ReadWord(kCityPending) == 0 &&
             PlayerPosition(xyz);
    }
    case Condition::kMemory: {
      uint32_t address, raw;
      if (!EvalAddress(c.address, address) || !ReadU32(address, raw)) {
        return false;
      }
      double value = raw;
      if (c.is_float) {
        float f;
        std::memcpy(&f, &raw, 4);
        value = f;
      }
      bool ok;
      return Compare(value, c.op, c.value, ok);
    }
  }
  return false;
}

std::string FormatPad(const PadValues& v) {
  char text[128];
  std::snprintf(text, sizeof text, "buttons=%04X lt=%u rt=%u lx=%d ly=%d rx=%d ry=%d",
                v.buttons, v.lt, v.rt, v.lx, v.ly, v.rx, v.ry);
  return text;
}

void RequestQuit() {
  StopRecording();
  CloseFrameLog();
  if (g_window) {
    rex::ui::Window* window = g_window;
    window->app_context().CallInUIThread([window] { window->RequestClose(); });
  }
}

std::string RunScriptFile(const std::string& path);

// Runs one command; returns the reply ("ok ..." or "err ...").
std::string Execute(const std::string& line) {
  std::istringstream in(line);
  std::vector<std::string> args;
  for (std::string word; in >> word;) {
    if (word[0] == '#') {
      break;
    }
    args.push_back(word);
  }
  if (args.empty()) {
    return "ok";
  }
  std::string cmd = args[0];
  auto arg = [&](size_t i) { return i < args.size() ? args[i] : std::string(); };
  // Arguments i..last as written, for file names with spaces in them.
  auto rest = [&](size_t i, size_t last) {
    std::string joined;
    for (size_t j = i; j <= last && j < args.size(); j++) {
      joined += (j > i ? " " : "") + args[j];
    }
    return joined;
  };
  auto duration_arg = [&](size_t i, double fallback_ms, double& amount, bool& frames) {
    if (i >= args.size()) {
      amount = fallback_ms;
      frames = false;
      return true;
    }
    return ParseDuration(args[i], amount, frames);
  };

  if (cmd == "press" || cmd == "hold" || cmd == "down" || cmd == "up") {
    const uint16_t bit = ButtonBit(arg(1));
    if (!bit) {
      return "err unknown button '" + arg(1) + "'";
    }
    if (cmd == "down" || cmd == "up") {
      EditPad([&](PadValues& v) { v.buttons = cmd == "down" ? (v.buttons | bit) : (v.buttons & ~bit); });
      return "ok";
    }
    double amount;
    bool frames;
    if ((cmd == "hold" && args.size() < 3) || !duration_arg(2, 150, amount, frames)) {
      return "err duration";
    }
    EditPad([&](PadValues& v) { v.buttons |= bit; });
    Wait(amount, frames);
    EditPad([&](PadValues& v) { v.buttons &= ~bit; });
    return "ok";
  }
  if (cmd == "trigger" || cmd == "stick") {
    double value;
    if (!ParseNumber(arg(2), value)) {
      return "err value";
    }
    const std::string which = arg(1);
    auto apply = [&](double x) {
      EditPad([&](PadValues& v) {
        if (cmd == "trigger") {
          const auto t = static_cast<uint8_t>(std::clamp(x, 0.0, 1.0) * 255.0 + 0.5);
          (which == "lt" ? v.lt : v.rt) = t;
        } else {
          const auto s = static_cast<int16_t>(std::clamp(x, -1.0, 1.0) * 32767.0);
          if (which == "lx") v.lx = s;
          if (which == "ly") v.ly = s;
          if (which == "rx") v.rx = s;
          if (which == "ry") v.ry = s;
        }
      });
    };
    if ((cmd == "trigger" && which != "lt" && which != "rt") ||
        (cmd == "stick" && which != "lx" && which != "ly" && which != "rx" && which != "ry")) {
      return "err unknown axis '" + which + "'";
    }
    apply(value);
    if (args.size() > 3) {
      double amount;
      bool frames;
      if (!ParseDuration(args[3], amount, frames)) {
        return "err duration";
      }
      Wait(amount, frames);
      apply(0.0);
    }
    return "ok";
  }
  if (cmd == "release") {
    const std::string which = arg(1);
    if (which.empty() || which == "all") {
      SetPad({});
      return "ok";
    }
    if (const uint16_t bit = ButtonBit(which)) {
      EditPad([&](PadValues& v) { v.buttons &= ~bit; });
      return "ok";
    }
    EditPad([&](PadValues& v) {
      if (which == "lt") v.lt = 0;
      if (which == "rt") v.rt = 0;
      if (which == "lx") v.lx = 0;
      if (which == "ly") v.ly = 0;
      if (which == "rx") v.rx = 0;
      if (which == "ry") v.ry = 0;
    });
    return "ok";
  }
  if (cmd == "wait") {
    double amount;
    bool frames;
    if (!ParseDuration(arg(1), amount, frames)) {
      return "err duration";
    }
    Wait(amount, frames);
    return "ok frame=" + std::to_string(g_frame.load());
  }
  if (cmd == "wait_until" || cmd == "press_until") {
    // wait_until <condition> [timeout]
    // press_until <button> <condition> [every=2s] [hold=300ms] [timeout=120s]
    // Conditions: left_title, loading, roam, frames N, u32|f32 <addr> <op> <value>.
    const size_t first = cmd == "press_until" ? 2 : 1;
    const uint16_t bit = cmd == "press_until" ? ButtonBit(arg(1)) : 0;
    if (cmd == "press_until" && !bit) {
      return "err unknown button '" + arg(1) + "'";
    }
    Condition condition;
    size_t next = first;
    std::string error;
    if (!ParseCondition(args, next, condition, error)) {
      return "err " + error;
    }
    double timeout_ms = 120000, every_ms = 2000, hold_ms = 300;
    for (size_t i = next; i < args.size(); i++) {
      std::string key = "timeout", value = args[i];
      if (const size_t eq = value.find('='); eq != std::string::npos) {
        key = value.substr(0, eq);
        value = value.substr(eq + 1);
      }
      double amount;
      bool frames;
      if (!ParseDuration(value, amount, frames) || frames) {
        return "err duration '" + args[i] + "'";
      }
      (key == "every" ? every_ms : key == "hold" ? hold_ms : timeout_ms) = amount;
    }
    const auto start = Clock::now();
    auto elapsed_ms = [&] {
      return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    int presses = 0;
    auto last_press = start - std::chrono::hours(1);
    while (!Evaluate(condition)) {
      if (elapsed_ms() > timeout_ms) {
        return "err timeout after " + std::to_string(presses) + " presses frame=" +
               std::to_string(g_frame.load());
      }
      if (bit && std::chrono::duration<double, std::milli>(Clock::now() - last_press).count() >=
                     every_ms) {
        last_press = Clock::now();
        presses++;
        EditPad([&](PadValues& v) { v.buttons |= bit; });
        Wait(hold_ms, false);
        EditPad([&](PadValues& v) { v.buttons &= ~bit; });
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    char text[96];
    std::snprintf(text, sizeof text, "ok after %.1f s, %d presses, frame=", elapsed_ms() / 1000.0,
                  presses);
    return text + std::to_string(g_frame.load());
  }
  if (cmd == "pos") {
    float xyz[3];
    if (!PlayerPosition(xyz)) {
      return "err no player vehicle";
    }
    char text[96];
    std::snprintf(text, sizeof text, "ok %.2f %.2f %.2f", xyz[0], xyz[1], xyz[2]);
    return text;
  }
  if (cmd == "peek" || cmd == "peeku" || cmd == "peekf") {
    uint32_t address;
    if (!EvalAddress(arg(1), address)) {
      return "err address";
    }
    double count = cmd == "peek" ? 16 : 4;
    if (args.size() > 2 && !ParseNumber(arg(2), count)) {
      return "err count";
    }
    const size_t bytes = cmd == "peek" ? size_t(count) : size_t(count) * 4;
    const uint8_t* p = GuestPointer(address, std::max<size_t>(bytes, 1));
    if (!p || bytes > 4096) {
      return "err unreadable";
    }
    char text[32];
    std::snprintf(text, sizeof text, "ok %08X:", address);
    std::string reply = text;
    for (size_t i = 0; i < bytes; i += cmd == "peek" ? 1 : 4) {
      if (cmd == "peek") {
        std::snprintf(text, sizeof text, " %02X", p[i]);
      } else {
        const uint32_t raw = (uint32_t(p[i]) << 24) | (uint32_t(p[i + 1]) << 16) |
                             (uint32_t(p[i + 2]) << 8) | p[i + 3];
        if (cmd == "peeku") {
          std::snprintf(text, sizeof text, " %08X", raw);
        } else {
          float f;
          std::memcpy(&f, &raw, 4);
          std::snprintf(text, sizeof text, " %g", f);
        }
      }
      reply += text;
    }
    return reply;
  }
  if (cmd == "dump") {
    // dump <addr> <bytes> <file>: raw guest memory, for finding state variables.
    uint32_t address;
    double length;
    if (!EvalAddress(arg(1), address) || !ParseNumber(arg(2), length) || arg(3).empty()) {
      return "err dump <addr> <bytes> <file>";
    }
    const uint8_t* p = GuestPointer(address, static_cast<size_t>(length));
    if (!p) {
      return "err unreadable";
    }
    FILE* f = std::fopen(rest(3, args.size() - 1).c_str(), "wb");
    if (!f) {
      return "err cannot write " + arg(3);
    }
    std::fwrite(p, 1, static_cast<size_t>(length), f);
    std::fclose(f);
    return "ok";
  }
  if (cmd == "mark") {
    const std::string name = arg(1).empty() ? "mark" : arg(1);
    WriteMark(name);
    RecordMark(name);
    REXLOG_INFO("dev pad: mark {} at engine frame {}, swap {}", name, g_frame.load(),
                g_swaps.load());
    return "ok frame=" + std::to_string(g_frame.load()) + " swap=" + std::to_string(g_swaps.load());
  }
  if (cmd == "shot") {
    double scale = 1.0;
    size_t last = args.size() - 1;
    if (args.size() > 2 && ParseNumber(args[last], scale)) {
      last--;
    }
    std::string result;
    return Shot(rest(1, last), scale, result) ? "ok " + result : "err " + result;
  }
  if (cmd == "state") {
    const uint64_t elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count());
    return "ok frame=" + std::to_string(g_frame.load()) + " swaps=" +
           std::to_string(g_swaps.load()) + " ms=" + std::to_string(elapsed) +
           " foreign_frame_calls=" + std::to_string(g_foreign_frame_calls.load()) +
           " playing=" + std::to_string(g_playing.load()) +
           " state_polls=" + std::to_string(g_state_polls.load()) +
           " key_polls=" + std::to_string(g_key_polls.load()) +
           " keys_sent=" + std::to_string(g_keys_sent.load()) + " " + FormatPad(GetPad());
  }
  if (cmd == "log") {
    REXLOG_INFO("dev pad: {}", line.substr(line.find("log") + 3));
    return "ok";
  }
  if (cmd == "play") {
    std::string error;
    const bool has_from = args.size() > 2 && args.back().rfind("from=", 0) == 0;
    const std::string file = rest(1, args.size() - (has_from ? 2 : 1));
    auto rec = LoadRecording(file, error);
    if (!rec) {
      return "err " + error;
    }
    uint64_t base = 0;
    if (has_from) {
      const std::string mark = args.back().substr(5);
      auto it = std::find_if(rec->marks.begin(), rec->marks.end(),
                             [&](const auto& m) { return m.second == mark; });
      if (it == rec->marks.end()) {
        return "err no mark '" + mark + "' in " + file;
      }
      base = it->first;
    }
    {
      std::lock_guard lock(g_play_mutex);
      g_play = rec;
      // The recording's frame N (counted from the mark) is applied on the
      // N-th engine frame from now, as the recorder saw it when recording.
      g_play_start = g_frame.load();
      g_play_base = base;
      g_play_index = 0;
      g_play_pos_index = 0;
      g_play_max_dev = 0;
      g_play_pos_checked = 0;
      g_playing = true;
    }
    const uint64_t frames = rec->end - base;
    while (g_playing.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    char text[128];
    {
      std::lock_guard lock(g_play_mutex);
      std::snprintf(text, sizeof text, "ok played=%llu frames, %d positions checked, max_dev=%.2f m",
                    static_cast<unsigned long long>(frames), g_play_pos_checked, g_play_max_dev);
    }
    REXLOG_INFO("dev pad: {}", text + 3);
    return text;
  }
  if (cmd == "record") {
    if (arg(1) == "start") {
      std::string error;
      return StartRecording(rest(2, args.size() - 1), error) ? "ok" : "err " + error;
    }
    if (arg(1) == "stop") {
      StopRecording();
      return "ok";
    }
    return "err record start <file> | record stop";
  }
  if (cmd == "quit") {
    RequestQuit();
    return "ok";
  }
  if (cmd == "include") {
    return RunScriptFile(rest(1, args.size() - 1));
  }
  return "err unknown command '" + cmd + "'";
}

// --- The synthetic device ---

constexpr auto kScriptPadId = static_cast<rex::input::DeviceId>(0x53435044);  // "SCPD"

class ScriptPad final : public rex::input::InputDriver {
 public:
  ScriptPad() : InputDriver(nullptr, 2) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kScriptPadId;
    info.name = "ScriptPad";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, X_INPUT_STATE* out_state) override {
    if (id != kScriptPadId) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!g_in_record_poll) {
      g_poll_thread = ThisThread();
    }
    if (out_state) {
      std::lock_guard lock(g_pad_mutex);
      std::memset(out_state, 0, sizeof(*out_state));
      // Merged states keep the highest packet number, so this one stays above
      // a real pad's and changes with every poll: the game always reads it.
      out_state->packet_number = 0x40000000u + static_cast<uint32_t>(g_state_polls++);
      out_state->gamepad.buttons = g_pad.buttons;
      out_state->gamepad.left_trigger = g_pad.lt;
      out_state->gamepad.right_trigger = g_pad.rt;
      out_state->gamepad.thumb_lx = g_pad.lx;
      out_state->gamepad.thumb_ly = g_pad.ly;
      out_state->gamepad.thumb_rx = g_pad.rx;
      out_state->gamepad.thumb_ry = g_pad.ry;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t /*flags*/,
                                 X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kScriptPadId) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
      out_caps->vibration.left_motor_speed = 0xFFFF;
      out_caps->vibration.right_motor_speed = 0xFFFF;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, X_INPUT_VIBRATION*) override {
    return id == kScriptPadId ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Key events from button edges, as the SDL driver makes them (menus may read
  // XamInputGetKeystrokeEx rather than the pad state); no auto-repeat.
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t /*flags*/,
                              X_INPUT_KEYSTROKE* out) override {
    using rex::ui::VirtualKey;
    if (id != kScriptPadId) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!out) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    static constexpr VirtualKey kKeys[22] = {
        VirtualKey::kXInputPadDpadUp,      VirtualKey::kXInputPadDpadDown,
        VirtualKey::kXInputPadDpadLeft,    VirtualKey::kXInputPadDpadRight,
        VirtualKey::kXInputPadStart,       VirtualKey::kXInputPadBack,
        VirtualKey::kXInputPadLThumbPress, VirtualKey::kXInputPadRThumbPress,
        VirtualKey::kXInputPadLShoulder,   VirtualKey::kXInputPadRShoulder,
        VirtualKey::kNone,                 VirtualKey::kNone,
        VirtualKey::kXInputPadA,           VirtualKey::kXInputPadB,
        VirtualKey::kXInputPadX,           VirtualKey::kXInputPadY,
        VirtualKey::kXInputPadLTrigger,    VirtualKey::kXInputPadRTrigger,
        VirtualKey::kXInputPadLThumbUp,    VirtualKey::kXInputPadLThumbDown,
        VirtualKey::kXInputPadLThumbRight, VirtualKey::kXInputPadLThumbLeft,
    };
    g_key_polls++;
    const PadValues v = GetPad();
    uint32_t keys = v.buttons;
    keys |= uint32_t(v.lt > 30) << 16;
    keys |= uint32_t(v.rt > 30) << 17;
    keys |= uint32_t(v.ly > 16384) << 18;
    keys |= uint32_t(v.ly < -16384) << 19;
    keys |= uint32_t(v.lx > 16384) << 20;
    keys |= uint32_t(v.lx < -16384) << 21;
    std::lock_guard lock(keys_mutex_);
    const uint32_t changed = keys ^ last_keys_;
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < 22; i++) {
        const uint32_t bit = 1u << i;
        if (!(changed & bit) || kKeys[i] == VirtualKey::kNone) {
          continue;
        }
        const bool pressed = keys & bit;
        if (pass == 0 && pressed) {
          continue;  // key-ups first
        }
        out->virtual_key = uint16_t(kKeys[i]);
        out->unicode = 0;
        out->user_index = 0;
        out->hid_code = 0;
        out->flags = pressed ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
        last_keys_ ^= bit;
        g_keys_sent++;
        return X_ERROR_SUCCESS;
      }
    }
    last_keys_ = keys;
    return X_ERROR_EMPTY;
  }

 private:
  std::mutex keys_mutex_;
  uint32_t last_keys_ = 0;
};

// --- Command sources ---

// Directory of the script being run, for relative includes.
thread_local std::string g_script_dir;

std::string RunScriptFile(const std::string& name) {
  std::string path = name;
  const bool absolute = name.size() > 1 && (name[1] == ':' || name[0] == '\\' || name[0] == '/');
  if (!g_script_dir.empty() && !absolute) {
    path = g_script_dir + "\\" + name;
  }
  std::ifstream in(path);
  if (!in) {
    return "err cannot read " + path;
  }
  const std::string saved_dir = g_script_dir;
  g_script_dir = path.substr(0, path.find_last_of("\\/"));
  int number = 0;
  int errors = 0;
  for (std::string line; std::getline(in, line);) {
    number++;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string reply = Execute(line);
    if (reply.rfind("err", 0) == 0) {
      errors++;
      REXLOG_WARN("dev pad: {}:{}: {} -> {}", path, number, line, reply);
    } else if (line.find_first_not_of(" \t") != std::string::npos &&
               line[line.find_first_not_of(" \t")] != '#') {
      REXLOG_INFO("dev pad: {} -> {}", line, reply);
    }
  }
  g_script_dir = saved_dir;
  return errors ? "err " + std::to_string(errors) + " failed lines in " + path : "ok";
}

void RunScript(std::string path) {
  REXLOG_INFO("dev pad: running script {}", path);
  const std::string reply = RunScriptFile(path);
  REXLOG_INFO("dev pad: script {} done: {}", path, reply);
  if (REXCVAR_GET(mcla_pad_exit_on_end)) {
    RequestQuit();
  }
}

#if defined(_WIN32)
void ServePipe(std::string name) {
  const std::string path = "\\\\.\\pipe\\" + name;
  for (;;) {
    HANDLE pipe = CreateNamedPipeA(
        path.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
        65536, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      REXLOG_ERROR("dev pad: cannot create {} ({})", path, GetLastError());
      return;
    }
    if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
      std::string pending;
      char buffer[4096];
      DWORD read = 0;
      while (ReadFile(pipe, buffer, sizeof buffer, &read, nullptr) && read) {
        pending.append(buffer, read);
        size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
          std::string line = pending.substr(0, newline);
          pending.erase(0, newline + 1);
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          const std::string reply = Execute(line) + "\n";
          DWORD written = 0;
          WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &written, nullptr);
        }
      }
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  }
}
#endif

}  // namespace

// Called from MclaApp::OnPreSetup: adds the synthetic device when a pad
// option is set; otherwise the input system is the SDK's own.
void MclaDevPadConfigure(rex::RuntimeConfig& config) {
  g_enabled = !REXCVAR_GET(mcla_pad_pipe).empty() || !REXCVAR_GET(mcla_pad_script).empty() ||
              !REXCVAR_GET(mcla_pad_record).empty() || !REXCVAR_GET(mcla_pad_frames_csv).empty();
  if (!g_enabled) {
    return;
  }
  auto inner = config.input_factory;
  config.input_factory =
      [inner](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = inner ? inner(tool_mode) : nullptr;
    if (system && !tool_mode) {
      static_cast<rex::input::InputSystem*>(system.get())
          ->AddDriver(std::make_unique<ScriptPad>());
    }
    return system;
  };
}

// Called from MclaApp::OnPostSetup.
void MclaDevPadStart(rex::ui::Window* window) {
  if (!g_enabled) {
    return;
  }
  g_window = window;
  g_start = Clock::now();
  if (const std::string path = REXCVAR_GET(mcla_pad_frames_csv); !path.empty()) {
    g_frames_csv = std::fopen(path.c_str(), "w");
    if (g_frames_csv) {
      std::setvbuf(g_frames_csv, nullptr, _IOFBF, 1 << 20);
      std::fprintf(g_frames_csv, "kind,a,b,c\n");
    } else {
      REXLOG_ERROR("dev pad: cannot write {}", path);
    }
  }
  if (const std::string path = REXCVAR_GET(mcla_pad_record); !path.empty()) {
    std::string error;
    if (!StartRecording(path, error)) {
      REXLOG_ERROR("dev pad: {}", error);
    }
  }
#if defined(_WIN32)
  if (const std::string name = REXCVAR_GET(mcla_pad_pipe); !name.empty()) {
    std::thread(ServePipe, name).detach();
  }
#endif
  if (const std::string path = REXCVAR_GET(mcla_pad_script); !path.empty()) {
    std::thread(RunScript, path).detach();
  }
  std::atexit([] {
    StopRecording();
    CloseFrameLog();
  });
}

// From mcla_frame_delta, the game's update timer: counts engine frames, plays
// and records, and applies mcla_fixed_dt (ticks in r8).
void MclaDevPadFrame(uint64_t& ticks) {
  // The timer has callers on other threads; the first one is the game loop.
  static const std::thread::id owner = std::this_thread::get_id();
  if (std::this_thread::get_id() != owner) {
    if (g_enabled) {
      g_foreign_frame_calls.fetch_add(1, std::memory_order_relaxed);
    }
    return;
  }
  if (const int32_t fixed = REXCVAR_GET(mcla_fixed_dt); fixed > 0) {
    ticks = rex::chrono::Clock::guest_tick_frequency() / static_cast<uint64_t>(fixed);
  }
  if (!g_enabled) {
    return;
  }
  uint64_t frame;
  {
    std::lock_guard lock(g_frame_mutex);
    frame = g_frame.fetch_add(1) + 1;
  }
  g_frame_cv.notify_all();
  if (g_playing.load(std::memory_order_relaxed)) {
    PlayFrame(frame);
  }
  RecordFrame(frame);
}

// From mcla_present_interval: one call per guest swap.
void MclaDevPadSwap() {
  if (!g_enabled) {
    return;
  }
  const uint64_t swap = g_swaps.fetch_add(1) + 1;
  const auto now = Clock::now();
  std::lock_guard lock(g_log_mutex);
  if (g_frames_csv) {
    const auto since_start =
        std::chrono::duration_cast<std::chrono::microseconds>(now - g_start).count();
    const auto delta = g_last_swap.time_since_epoch().count()
                           ? std::chrono::duration_cast<std::chrono::microseconds>(now - g_last_swap)
                                 .count()
                           : 0;
    std::fprintf(g_frames_csv, "s,%lld,%lld,%llu\n", static_cast<long long>(since_start),
                 static_cast<long long>(delta), static_cast<unsigned long long>(g_frame.load()));
  }
  g_last_swap = now;
  (void)swap;
}

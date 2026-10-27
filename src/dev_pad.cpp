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
//   wait 2s | 500ms | 90f (engine frames) | 300ms 18f (until both passed)
//   wait_until <condition> [timeout]
//   press_until <button> <condition> [every=2s] [hold=300ms] [timeout=120s]
//   pos   peek <addr> [bytes]   peeku <addr> [count]   peekf <addr> [count]
//   poke <addr> <u32> (a big-endian word, for experiments)
//   saving [wait] (whether a career save is being written, as closing sees
//     it; with wait, until one is, for up to 10 s)
//   close (as the window's close button: unlike quit, it waits for a save)
//   dump <addr> <bytes> <file>   mark <name>   shot <file.png> [scale]
//     (mcla_pad_timeout_shot=<file.png>: the same picture when a wait_until or
//     press_until times out)
//   screen <file.png> [scale] (Windows: the game window's client area as the screen shows
//     it, the presenter's scaling filter (present_effect) and the UI over the picture
//     included; shot saves the guest output before them. Needs the display on)
//   state   log <text>   include <script>   quit   set <option> <value>
//   window minimize|restore|hide|show|cover|uncover|state (Windows: the game's own window,
//     for checking what a minimised, hidden or covered window does to the frame loop)
//   get <option>... (the options' values: the reply, and the log; fg_status is
//     frame generation's state, the presenter's, not an option; so is ll_status, the low
//     latency feature's)
//   save_config (what the F4 overlay's "Save to config" does: rex::cvar::SaveConfig
//     into mcla.toml next to the executable)
//   write_content_header <name> (a content header file, as the game's saves get, from a
//     struct whose padding holds 0xEE: rexglue-content-header-padding.patch writes zeros)
//   play <recording.csv> [from=<mark>]   record start <file.csv> | record stop
//   cars (race cars' positions)   watch <every> | watch stop (log them)
//   screens (the UI manager's movies)
//   follow <route> [speed=1] | follow stop | follow status (drive a route; a car knocked off it
//     is brought back, see "Route following")
//   autocontinue on | off | status (press A when a dialog over the race stops the world)
//   findvec <addr> [floats] [tolerance] (where else in guest memory the same floats are)
//
// Conditions: left_title (Start accepted on the title screen), loading (the
// map zoom-out), roam (the player is in the city), frames N (engine frames
// since start-up), u32|f32 <addr> ==|!=|<|>|<=|>= <value>. Addresses are guest
// addresses, with [x] for "the 32-bit value at x" and +/- offsets:
// [0x8288DCF8]+0x2A0.
//
// The game ignores buttons during its own transitions (the title screen's
// camera cuts, a menu closing), so scripts press until a condition holds.
// Menus that move in real time but animate per update need both, as at low
// frame rates (validation layers) a wait in time alone lets too few updates
// pass. A line that fails ends the script, and the run with
// mcla_pad_exit_on_end, rather than the rest running out of step.
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
#include <cstdlib>
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
#include <rex/filesystem.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/achievement_manager.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/low_latency.h>
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
REXCVAR_DEFINE_STRING(mcla_pad_timeout_shot, "", "MCLA",
                      "Development: when a wait_until or press_until times out, save a screenshot "
                      "to this PNG before the script fails (devrun.py sets it to the run's folder)");
REXCVAR_DEFINE_INT32(mcla_fixed_dt, 0, "MCLA",
                     "Development: every game update advances exactly 1/N s (0 = measured "
                     "time). Uncapped this is a timedemo; for A/B measurements only")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(mcla_freeze, false, "MCLA",
                    "Development: every game update advances one timer tick (20 ns), so the "
                    "world stands still while frames are still drawn; for comparing frames "
                    "of one moment (A/B of rendering options, temporal AA convergence)");

bool MclaSaveInProgress();  // src/save_wait.cpp

uint64_t MclaPumpRounds();  // src/poll_wait.cpp: rounds of the thread that frees the command ring's slots

namespace {

using namespace rex;
using namespace rex::input;
using Clock = std::chrono::steady_clock;

bool g_enabled = false;
rex::ui::Window* g_window = nullptr;
rex::ui::ImGuiDrawer* g_imgui_drawer = nullptr;

// testdialog: a dialog that closes itself from inside its own draw once its
// time is up, the way a XAM message box closes from its OK button.
class SelfClosingTestDialog final : public rex::ui::ImGuiDialog {
 public:
  SelfClosingTestDialog(rex::ui::ImGuiDrawer* drawer, double seconds)
      : rex::ui::ImGuiDialog(drawer),
        until_(std::chrono::steady_clock::now() +
               std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                   std::chrono::duration<double>(seconds))) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    ImGui::SetNextWindowPos(ImVec2(80, 80), ImGuiCond_Always);
    ImGui::Begin("dev pad test dialog", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("This dialog closes itself from its own draw.");
    ImGui::End();
    if (std::chrono::steady_clock::now() >= until_) {
      Close();
    }
  }

 private:
  std::chrono::steady_clock::time_point until_;
};

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

#if defined(_WIN32)
// The game's own window (the process's largest top-level one), for the `window` command.
HWND g_game_hwnd = nullptr;
// The cover's thread is detached (a joinable static thread would end the process in
// std::terminate if the run ends while it is up); uncover tells it to quit.
std::atomic<bool> g_cover_active{false};
std::atomic<DWORD> g_cover_thread_id{0};
std::atomic<bool> g_cover_ready{false};

HWND FindGameWindow() {
  if (g_game_hwnd && IsWindow(g_game_hwnd)) {
    return g_game_hwnd;
  }
  struct Search {
    DWORD pid;
    HWND best;
    LONG area;
  } search{GetCurrentProcessId(), nullptr, 0};
  EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        Search* s = reinterpret_cast<Search*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        RECT rect;
        if (pid != s->pid || GetWindow(hwnd, GW_OWNER) || !IsWindowVisible(hwnd) ||
            !GetWindowRect(hwnd, &rect)) {
          return TRUE;
        }
        const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
        if (area > s->area) {
          s->area = area;
          s->best = hwnd;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  g_game_hwnd = search.best;
  return g_game_hwnd;
}

std::string WindowCommand(const std::string& what) {
  HWND hwnd = FindGameWindow();
  if (!hwnd) {
    return "err no window";
  }
  if (what == "minimize") {
    ShowWindow(hwnd, SW_MINIMIZE);
  } else if (what == "restore") {
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
  } else if (what == "hide") {
    ShowWindow(hwnd, SW_HIDE);
  } else if (what == "show") {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
  } else if (what == "cover") {
    // A black pop-up of ours over the window (not activated): the window is occluded, as by
    // another program's window, and loses the focus the way an alt-tab does when it is clicked.
    if (g_cover_active) {
      return "err already covered";
    }
    RECT rect;
    GetWindowRect(hwnd, &rect);
    g_cover_ready = false;
    g_cover_active = true;
    std::thread([rect] {
      g_cover_thread_id = GetCurrentThreadId();
      HWND cover = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"",
                                   WS_POPUP | WS_VISIBLE | SS_BLACKRECT, rect.left, rect.top,
                                   rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                   nullptr, nullptr);
      SetForegroundWindow(cover);
      g_cover_ready = true;
      MSG msg;
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      DestroyWindow(cover);
      g_cover_active = false;
    }).detach();
    for (int i = 0; i < 200 && !g_cover_ready; ++i) {
      Sleep(10);
    }
  } else if (what == "uncover") {
    if (!g_cover_active) {
      return "err not covered";
    }
    PostThreadMessageW(g_cover_thread_id, WM_QUIT, 0, 0);
    for (int i = 0; i < 200 && g_cover_active; ++i) {
      Sleep(10);
    }
    SetForegroundWindow(hwnd);
  } else if (what != "state") {
    return "err usage: window minimize|restore|hide|show|cover|uncover|state";
  }
  const std::string state = std::string("iconic=") + (IsIconic(hwnd) ? "1" : "0") +
                            " visible=" + (IsWindowVisible(hwnd) ? "1" : "0") +
                            " foreground=" + (GetForegroundWindow() == hwnd ? "1" : "0");
  REXLOG_INFO("dev pad: window {}: {}", what, state);
  return "ok " + state;
}

// The game window's client area as the desktop shows it (a copy from the screen, so the
// window is put above the others for the moment), for looking at the presenter's scaling
// filter, which `shot` doesn't have: it saves the guest output before the presenter.
bool ScreenShot(const std::string& path, double scale, std::string& result) {
  HWND hwnd = FindGameWindow();
  if (!hwnd) {
    result = "no window";
    return false;
  }
  // The window's pixels, not a DPI-scaled size, whatever the process is aware of.
  using SetThreadDpiFn = HANDLE(WINAPI*)(HANDLE);
  SetThreadDpiFn set_dpi = reinterpret_cast<SetThreadDpiFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext")));
  HANDLE old_dpi = set_dpi ? set_dpi(reinterpret_cast<HANDLE>(intptr_t(-4))) : nullptr;
  RECT client = {};
  POINT origin = {0, 0};
  GetClientRect(hwnd, &client);
  ClientToScreen(hwnd, &origin);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  bool ok = width > 0 && height > 0;
  std::vector<uint8_t> bgra;
  if (ok) {
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Sleep(300);
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib && bits) {
      HGDIOBJ previous = SelectObject(memory, dib);
      ok = BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT) != 0;
      GdiFlush();
      if (ok) {
        bgra.assign(static_cast<const uint8_t*>(bits), static_cast<const uint8_t*>(bits) + size_t(width) * height * 4);
      }
      SelectObject(memory, previous);
    } else {
      ok = false;
    }
    if (dib) {
      DeleteObject(dib);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  if (set_dpi && old_dpi) {
    set_dpi(old_dpi);
  }
  if (!ok) {
    result = "the screen copy failed";
    return false;
  }
  scale = std::clamp(scale, 0.05, 1.0);
  const uint32_t out_width = std::max<uint32_t>(1, uint32_t(width * scale));
  const uint32_t out_height = std::max<uint32_t>(1, uint32_t(height * scale));
  std::vector<uint8_t> rgb(size_t(out_width) * out_height * 3);
  for (uint32_t y = 0; y < out_height; y++) {
    const uint32_t sy = std::min<uint32_t>(height - 1, uint32_t((y + 0.5) / scale));
    for (uint32_t x = 0; x < out_width; x++) {
      const uint32_t sx = std::min<uint32_t>(width - 1, uint32_t((x + 0.5) / scale));
      const uint8_t* src = bgra.data() + (size_t(sy) * width + sx) * 4;
      uint8_t* dst = rgb.data() + (size_t(y) * out_width + x) * 3;
      dst[0] = src[2];
      dst[1] = src[1];
      dst[2] = src[0];
    }
  }
  if (!WritePng(path, out_width, out_height, rgb)) {
    result = "cannot write " + path;
    return false;
  }
  result = std::to_string(out_width) + "x" + std::to_string(out_height);
  return true;
}
#endif

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

// What a wait that timed out leaves behind: a picture of where the game was, so
// that a boot stuck for minutes can be told from a script that waited for the
// wrong thing (a stuck boot in a run once left nothing to look at). Appended to
// the failure's reply.
std::string TimeoutShot() {
  const std::string path = REXCVAR_GET(mcla_pad_timeout_shot);
  if (path.empty()) {
    return "";
  }
  std::string result;
  if (Shot(path, 1.0, result)) {
    return " shot=" + path + " (" + result + ")";
  }
  return " (no screenshot: " + result + ")";
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
// Seconds of game time since the current mode was loaded (a float): it
// starts again from 0 when a race loads (found the same way; its start is
// about 9.3 s on this timer after the race has loaded).
constexpr uint32_t kModeTimer = 0x827D7510;

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

bool ReadFloat(uint32_t address, float& value) {
  uint32_t raw;
  if (!ReadU32(address, raw)) {
    return false;
  }
  std::memcpy(&value, &raw, 4);
  return true;
}

// Where the player's car points, on the ground (x, z), unit length: the
// matrix's third row (+0x290) points backwards (checked by driving).
bool PlayerForward(float xz[2]) {
  uint32_t vehicle = 0;
  float x, z;
  if (!ReadU32(kPlayerVehicle, vehicle) || !vehicle ||
      !ReadFloat(vehicle + kVehiclePositionOffset - 0x10, x) ||
      !ReadFloat(vehicle + kVehiclePositionOffset - 0x08, z)) {
    return false;
  }
  const float length = std::sqrt(x * x + z * z);
  if (!(length > 0.1f)) {
    return false;
  }
  xz[0] = -x / length;
  xz[1] = -z / length;
  return true;
}

// --- Race cars ---

// The cars in a race are objects of the player's car's class (the same
// vtable): the player and the AI opponents; traffic is another class. An
// opponent's +0x2A0 keeps its place on the starting grid; its live matrix is
// at +0x440, with the position at +0x470 (found by diffing an opponent's
// object while it raced). The player's +0x470 is live too.
constexpr uint32_t kCarLivePositionOffset = 0x470;

std::vector<uint32_t> FindCars() {
  std::vector<uint32_t> cars;
  uint32_t player = 0, vtable = 0;
  if (!ReadU32(kPlayerVehicle, player) || !player || !ReadU32(player, vtable)) {
    return cars;
  }
  auto* runtime = rex::Runtime::instance();
  if (!runtime) {
    return cars;
  }
#if defined(_WIN32)
  // The 512 MB heap the player's car is in, region by region.
  const uint32_t low = player & 0xE0000000u;
  const uint64_t high = uint64_t(low) + 0x20000000u;
  uint8_t* const base = runtime->virtual_membase();
  const uint8_t want[4] = {uint8_t(vtable >> 24), uint8_t(vtable >> 16), uint8_t(vtable >> 8),
                           uint8_t(vtable)};
  uint64_t address = low;
  while (address < high) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(base + address, &info, sizeof info)) {
      break;
    }
    const uint64_t region_end = std::min<uint64_t>(
        high, uint64_t(static_cast<uint8_t*>(info.BaseAddress) - base) + info.RegionSize);
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      for (uint64_t a = (address + 15) & ~uint64_t(15); a + 4 <= region_end; a += 16) {
        if (std::memcmp(base + a, want, 4) == 0) {
          cars.push_back(static_cast<uint32_t>(a));
        }
      }
    }
    address = region_end;
  }
#endif
  // The player first.
  std::stable_partition(cars.begin(), cars.end(), [&](uint32_t car) { return car == player; });
  return cars;
}

// Every 4-aligned guest address (committed pages, the whole 4 GB) that holds these big-endian
// floats, each within `tolerance`.
std::vector<uint32_t> FindFloats(const float* want, size_t count, float tolerance) {
  std::vector<uint32_t> hits;
#if defined(_WIN32)
  auto* runtime = rex::Runtime::instance();
  if (!runtime || count < 1) {
    return hits;
  }
  uint8_t* const base = runtime->virtual_membase();
  auto word = [&](const uint8_t* q) {
    const uint32_t bits = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3];
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  };
  uint64_t address = 0;
  while (address < 0x100000000ull) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(base + address, &info, sizeof info)) {
      break;
    }
    const uint64_t region_end = std::min<uint64_t>(
        0x100000000ull, uint64_t(static_cast<uint8_t*>(info.BaseAddress) - base) + info.RegionSize);
    if (info.State == MEM_COMMIT && !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      for (uint64_t a = (address + 3) & ~uint64_t(3); a + 4 * count <= region_end; a += 4) {
        if (std::fabs(word(base + a) - want[0]) > tolerance) {
          continue;
        }
        bool all = true;
        for (size_t k = 1; all && k < count; k++) {
          all = std::fabs(word(base + a + 4 * k) - want[k]) <= tolerance;
        }
        if (all) {
          hits.push_back(static_cast<uint32_t>(a));
        }
      }
    }
    address = region_end;
  }
#else
  (void)want;
  (void)count;
  (void)tolerance;
#endif
  return hits;
}

// --- Route following ---

// follow <route> drives the player's car along a route, a file of "x,z,speed" lines (metres on
// the ground, m/s; a fourth column, the ground's height, is read and not used) made by
// launcher/dev/route.py: the stick steers at a point ahead on the route (pure pursuit), the
// triggers hold the route's speed (the buttons a script holds are left as they are). It runs in the
// frame hook, on the game's update thread, so with mcla_fixed_dt the same game state gives the same
// inputs.
//
// Shader coverage needs the run to travel the route, not to drive well, so a car that is knocked off
// it (traffic, a wall, an opponent) is brought back in steps, each one logged and counted:
// 1. a longer reverse each time, steering toward the route (the nose swings to it), straight back, or
//    on full lock one way and then the other, each followed by a forward burst on full lock toward
//    the route;
// 2. once the car is more than 30 m off, the nearest point of the route by distance, behind or ahead
//    (not only ahead), is the one it steers for;
// 3. (tried and left out: putting the car back on the route by writing its position and velocity. Its
//    state is in the vehicle object and in some 590 other places with the same floats, all written
//    here, and the car drove on as if nothing had been written: its real state is somewhere that
//    isn't a copy of those floats. The game has no reset of the car either: the pause menu in a
//    race has Restart Race, Next Race and Quit Race only. `findvec` is what looked for it.)
// 4. a car that makes no progress for 20 s of game time, however it moves, counts as stuck too.
// A run that still did not travel the route is a failed job (coverage.py: less than 80% of it).
// The summary says how far along the route the car got (best=) and how many recoveries it needed.
//
// When the whole world stands still once a car has moved (a tip or dialog over the race: "SLIP
// STREAM TURBO ... CONTINUE (A)"), A is pressed (TipsStep); in a race A is the handbrake, so a stray
// press does no harm.

std::mutex g_follow_mutex;
std::atomic<bool> g_following{false};

// Every race car's position, to tell the world standing still (a dialog the game stops everything
// for) from one car standing still (stuck). The cars are found once, when it starts: FindCars scans
// the heap.
struct Stillness {
  std::vector<uint32_t> cars;  // the player first
  std::vector<std::array<float, 3>> previous, origin;
  bool moved = false;       // a car has gone 20 m from where it first was
  bool still = false;       // the last reading: no car's position changed
  int still_frames = 0;     // updates in a row the world stood still, once a car had moved
  int press_frames = 0;     // A is held while this counts down
  int dismissed = 0;        // times A was pressed for it
};

void StartStillness(Stillness& s) {
  s = Stillness{};
  s.cars = FindCars();
  if (s.cars.empty()) {
    uint32_t player = 0;
    if (ReadU32(kPlayerVehicle, player) && player) {
      s.cars.push_back(player);
    }
  }
}

bool WorldStill(Stillness& s) {
  std::vector<std::array<float, 3>> now(s.cars.size());
  for (size_t i = 0; i < s.cars.size(); i++) {
    const uint32_t offset = i == 0 ? kVehiclePositionOffset : kCarLivePositionOffset;
    for (int k = 0; k < 3; k++) {
      if (!ReadFloat(s.cars[i] + offset + 4 * k, now[i][k])) {
        now[i][k] = 0;
      }
    }
  }
  if (s.origin.size() != now.size()) {
    s.origin = now;
  }
  bool still = !now.empty() && s.previous.size() == now.size();
  for (size_t i = 0; still && i < now.size(); i++) {
    still = std::memcmp(now[i].data(), s.previous[i].data(), sizeof(now[i])) == 0;
  }
  for (size_t i = 0; i < now.size(); i++) {
    const double d = std::hypot(now[i][0] - s.origin[i][0], now[i][2] - s.origin[i][2]);
    s.moved |= d > 20.0;
  }
  s.previous = now;
  s.still = still;
  return still;
}

// One update's reading: true while A should be down for a dialog.
bool TipsStep(Stillness& s) {
  WorldStill(s);
  s.still_frames = s.still && s.moved ? s.still_frames + 1 : 0;
  if (s.press_frames > 0) {
    s.press_frames--;
    return true;
  }
  if (s.still_frames >= 15) {
    s.still_frames = 0;
    s.press_frames = 6;
    s.dismissed++;
    REXLOG_INFO("dev pad: the world stands still (a tip or a dialog over the race): A pressed ({} so far)",
                s.dismissed);
    return true;
  }
  return false;
}

// autocontinue on | off: TipsStep for a script that doesn't follow a route (the follower does its
// own), so that a dialog doesn't stop the race it is watching.
std::atomic<bool> g_autocontinue{false};
Stillness g_tips;
bool g_tips_a_down = false;

void AutoContinueFrame() {
  if (g_following.load(std::memory_order_relaxed)) {
    return;
  }
  const bool down = TipsStep(g_tips);
  if (down != g_tips_a_down) {
    g_tips_a_down = down;
    EditPad([&](PadValues& v) { v.buttons = down ? (v.buttons | 0x1000) : (v.buttons & ~0x1000); });
  }
}

struct Route {
  std::vector<std::array<float, 2>> points;
  std::vector<float> speed;
  std::vector<float> height;  // NaN where the file has none
};

enum class Recovery { kDrive, kReverse, kForward };

struct Follower {
  std::shared_ptr<Route> route;
  double speed_scale = 1.0;
  size_t index = 0;
  size_t best = 0;           // the furthest point the car has been nearest to
  size_t progress_mark = 0;  // best when it last counted as progress
  bool have_prev = false;
  float prev[2] = {};
  double prev_dt = 0;
  double speed = 0;  // m/s over the last update
  bool moved = false;
  int slow_frames = 0;
  int still_run = 0;      // updates in a row nothing moved, A presses included
  int no_progress = 0;    // updates since the route's best point last advanced
  Recovery phase = Recovery::kDrive;
  int phase_frames = 0;
  int attempt = 0;        // recoveries in a row without progress
  int recoveries = 0;
  int retargets = 0;
  Stillness tips;
  double max_offset = 0;
  uint64_t frames = 0;
};

Follower g_follow;

std::shared_ptr<Route> LoadRoute(const std::string& path, std::string& error) {
  std::ifstream in(path);
  if (!in) {
    error = "cannot read " + path;
    return nullptr;
  }
  auto route = std::make_shared<Route>();
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line[0] == '#' || !(std::isdigit(uint8_t(line[0])) || line[0] == '-')) {
      continue;
    }
    for (char& c : line) {
      if (c == ',') {
        c = ' ';
      }
    }
    std::istringstream fields(line);
    float x, z, speed, height = std::nanf("");
    fields >> x >> z >> speed;
    if (fields.fail()) {
      error = "bad line: " + line;
      return nullptr;
    }
    if (!(fields >> height)) {
      height = std::nanf("");
    }
    route->points.push_back({x, z});
    route->speed.push_back(speed);
    route->height.push_back(height);
  }
  if (route->points.size() < 2) {
    error = "no route in " + path;
    return nullptr;
  }
  return route;
}

std::string FollowSummary(const Follower& f) {
  char text[240];
  const size_t last = f.route ? f.route->points.size() : size_t(0);
  std::snprintf(text, sizeof text,
                "frames=%llu point=%zu/%zu best=%zu/%zu max_offset=%.1f m recoveries=%d "
                "retargets=%d popups=%d speed=%.1f m/s",
                static_cast<unsigned long long>(f.frames), f.index, last, f.best, last, f.max_offset,
                f.recoveries, f.retargets, f.tips.dismissed, f.speed);
  return text;
}

void StopFollowing(const char* why) {
  std::lock_guard lock(g_follow_mutex);
  if (g_follow.route) {
    REXLOG_INFO("dev pad: follow {}: {}", why, FollowSummary(g_follow));
  }
  g_follow.route.reset();
  g_following = false;
  SetPad({});
}

void FollowFrame(double dt) {
  std::unique_lock lock(g_follow_mutex);
  Follower& f = g_follow;
  if (!f.route) {
    return;
  }
  const auto& points = f.route->points;
  float position[3], forward[2];
  if (!PlayerPosition(position) || !PlayerForward(forward)) {
    return;
  }
  const double px = position[0], pz = position[2];
  if (f.have_prev && f.prev_dt > 0) {
    const double moved = std::hypot(px - f.prev[0], pz - f.prev[1]);
    f.speed = moved / f.prev_dt;
    if (f.speed > 120) {
      f.speed = 0;  // put somewhere else: the race (re)starting, a reset
      f.slow_frames = 0;
    }
  }
  f.prev[0] = float(px);
  f.prev[1] = float(pz);
  f.prev_dt = dt;
  f.have_prev = true;
  f.frames++;
  f.moved |= f.speed > 5;
  f.tips.moved |= f.moved;
  const bool a_for_tip = TipsStep(f.tips);
  f.still_run = f.tips.still && f.moved ? f.still_run + 1 : 0;

  // The nearest point, looking only ahead (100 m), so a route that comes back past itself is not
  // cut short.
  auto distance = [&](size_t i) { return std::hypot(points[i][0] - px, points[i][1] - pz); };
  auto step = [&](size_t i) {
    return std::hypot(points[i + 1][0] - points[i][0], points[i + 1][1] - points[i][1]);
  };
  double nearest = distance(f.index);
  double along = 0;
  for (size_t i = f.index; i + 1 < points.size() && along < 100; i++) {
    along += step(i);
    if (const double d = distance(i + 1); d < nearest) {
      nearest = d;
      f.index = i + 1;
    }
  }
  if (nearest > 30) {
    // Knocked well off: the nearest point by distance within 600 m of where it was, behind or ahead.
    const size_t low = f.index > 150 ? f.index - 150 : 0;
    const size_t high = std::min(points.size() - 1, f.index + 300);
    size_t best = f.index;
    double best_distance = nearest;
    for (size_t i = low; i <= high; i++) {
      if (const double d = distance(i); d < best_distance - 1.0) {
        best_distance = d;
        best = i;
      }
    }
    if (best != f.index) {
      f.index = best;
      nearest = best_distance;
      f.retargets++;
    }
  }
  f.max_offset = std::max(f.max_offset, nearest);
  f.best = std::max(f.best, f.index);
  if (f.best >= f.progress_mark + 5) {
    f.progress_mark = f.best;
    f.attempt = 0;
    f.no_progress = 0;
  } else if (f.moved) {
    f.no_progress++;
  }
  if (f.index + 1 >= points.size()) {
    lock.unlock();
    StopFollowing("reached the end of the route");
    return;
  }

  // Steer at the point a speed-dependent distance ahead.
  const double look = std::clamp(6.0 + 0.6 * f.speed, 8.0, 40.0);
  size_t target = f.index;
  along = 0;
  while (target + 1 < points.size() && along < look) {
    along += step(target);
    target++;
  }
  const double tx = points[target][0] - px, tz = points[target][1] - pz;
  // Positive is to the right (stick right turns this way).
  const double angle =
      std::atan2(forward[0] * tz - forward[1] * tx, forward[0] * tx + forward[1] * tz);
  double steer = std::clamp(2.5 * angle, -1.0, 1.0);

  // The slowest route speed over the next 1.5 s (at least 30 m).
  double wanted = 1e9;
  along = 0;
  for (size_t i = f.index; i + 1 < points.size() && along < std::max(30.0, 1.5 * f.speed); i++) {
    wanted = std::min<double>(wanted, f.route->speed[i]);
    along += step(i);
  }
  wanted *= f.speed_scale;

  // Stuck: against something with the throttle open while the world moves (one car, the player,
  // standing still is not a dialog), or nowhere for a long while, or the world standing still for
  // longer than a dialog takes to answer.
  PadValues pad;
  constexpr uint16_t kButtonA = 0x1000;
  pad.buttons = GetPad().buttons & ~kButtonA;
  if (a_for_tip) {
    pad.buttons |= kButtonA;
  }
  auto start_recovery = [&](const char* why) {
    f.attempt++;
    f.recoveries++;
    f.slow_frames = 0;
    f.still_run = 0;
    f.no_progress = 0;
    // Reverse for longer each time, then a burst forward toward the route.
    f.phase = Recovery::kReverse;
    f.phase_frames = 60 + 25 * std::min(f.attempt, 8);
    REXLOG_WARN("dev pad: follow recovery {} ({}): attempt {}, reverse {} updates at point {} of {} ({:.1f} m off, "
                "{:.1f} m/s)",
                f.recoveries, why, f.attempt, f.phase_frames, f.index, points.size(), nearest, f.speed);
  };

  if (f.phase == Recovery::kReverse) {
    // Attempts take turns: toward the route (the nose swings to it), straight back, on full lock
    // one way, on full lock the other.
    switch (f.attempt % 4) {
      case 1: pad.lx = int16_t(-steer * 32767.0); break;
      case 2: pad.lx = 0; break;
      case 3: pad.lx = int16_t((angle > 0 ? -1.0 : 1.0) * 32767.0); break;
      default: pad.lx = int16_t((angle > 0 ? 1.0 : -1.0) * 32767.0); break;
    }
    pad.lt = 255;
    if (--f.phase_frames <= 0) {
      f.phase = Recovery::kForward;
      f.phase_frames = 45;
    }
    SetPad(pad);
    return;
  }
  if (f.phase == Recovery::kForward) {
    pad.rt = 200;
    pad.lx = static_cast<int16_t>(std::clamp(4.0 * angle, -1.0, 1.0) * 32767.0);
    if (--f.phase_frames <= 0) {
      f.phase = Recovery::kDrive;
    }
    SetPad(pad);
    return;
  }

  if (std::abs(angle) > 0.8 && f.speed > 12) {
    pad.lt = 200;  // well off line: slow down to turn
  } else if (f.speed > wanted + 6) {
    pad.lt = 255;  // slower ahead
  } else if (f.speed < wanted + 2) {
    pad.rt = 255;
  }
  f.slow_frames = pad.rt && f.moved && !f.tips.still && f.speed < 1.5 ? f.slow_frames + 1 : 0;
  if (f.slow_frames > 45) {
    start_recovery("pinned");
  } else if (f.still_run > 150) {
    start_recovery("still despite A");
  } else if (f.no_progress > 1200) {
    start_recovery("no progress for 20 s");
  }
  pad.lx = static_cast<int16_t>(steer * 32767.0);
  SetPad(pad);
}

// --- Watching the race cars ---

// watch <every> logs every race car's position each <every> updates of the
// mode timer, on the game's update thread, so runs can be compared at the same
// game time whatever frame a script's command lands on.
std::mutex g_watch_mutex;
std::vector<uint32_t> g_watch_cars;
int g_watch_every = 0;
int64_t g_watch_last = -1;
std::atomic<bool> g_watching{false};

void WatchFrame(uint64_t frame) {
  std::lock_guard lock(g_watch_mutex);
  float timer;
  if (!g_watch_every || !ReadFloat(kModeTimer, timer)) {
    return;
  }
  const int32_t fixed = REXCVAR_GET(mcla_fixed_dt);
  const int64_t tick = std::llround(double(timer) * (fixed > 0 ? fixed : 60));
  if (tick == g_watch_last || tick % g_watch_every != 0) {
    return;
  }
  g_watch_last = tick;
  std::string line = "dev pad: watch tick=" + std::to_string(tick) +
                     " frame=" + std::to_string(frame);
  for (const uint32_t car : g_watch_cars) {
    float xyz[3] = {};
    for (int i = 0; i < 3; i++) {
      ReadFloat(car + kCarLivePositionOffset + 4 * i, xyz[i]);
    }
    char text[64];
    std::snprintf(text, sizeof text, " | %.3f %.3f %.3f", xyz[0], xyz[1], xyz[2]);
    line += text;
  }
  REXLOG_INFO("{}", line);
}

// --- UI screens ---

// The UI manager's movies by name (atMap name -> movie**: buckets at +60,
// count in the high half of +64; node: +0 name, +8 movie**, +12 next). From
// LARecomp's notes, used with its author's permission (as in frame_timing.cpp).
constexpr uint32_t kUiManagerPtr = 0x8286D804;

std::vector<std::string> UiScreens() {
  std::vector<std::string> names;
  const uint32_t manager = ReadWord(kUiManagerPtr);
  if (manager == 0 || manager == 0xFFFFFFFF) {
    return names;
  }
  const uint32_t buckets = ReadWord(manager + 60);
  const uint32_t count = ReadWord(manager + 64) >> 16;
  for (uint32_t i = 0; buckets && i < count && i < 256; i++) {
    uint32_t node = ReadWord(buckets + 4 * i);
    for (int guard = 0; node && node != 0xFFFFFFFF && guard < 256;
         guard++, node = ReadWord(node + 12)) {
      const uint32_t name = ReadWord(node);
      const uint8_t* text = name ? GuestPointer(name, 1) : nullptr;
      if (!text) {
        continue;
      }
      std::string s;
      for (; s.size() < 64; text++) {
        if (!*text) {
          break;
        }
        s += char(*text);
      }
      names.push_back(s);
    }
  }
  return names;
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

// Directory of the script being run, for relative includes and routes.
thread_local std::string g_script_dir;

std::string ScriptRelative(const std::string& name) {
  const bool absolute = name.size() > 1 && (name[1] == ':' || name[0] == '\\' || name[0] == '/');
  return !g_script_dir.empty() && !absolute ? g_script_dir + "\\" + name : name;
}

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
    if (arg(2).empty()) {
      Wait(amount, frames);
      return "ok frame=" + std::to_string(g_frame.load());
    }
    // A time and a frame count: until both have passed.
    double amount2;
    bool frames2;
    if (!ParseDuration(arg(2), amount2, frames2) || frames2 == frames) {
      return "err wait <time> <frames>f";
    }
    const uint64_t target = g_frame.load() + static_cast<uint64_t>(frames ? amount : amount2);
    Wait(frames ? amount2 : amount, false);
    const uint64_t now = g_frame.load();
    if (now < target) {
      WaitFrames(target - now);
    }
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
               std::to_string(g_frame.load()) + TimeoutShot();
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
  if (cmd == "saving") {
    if (arg(1) == "wait") {
      const auto start = std::chrono::steady_clock::now();
      while (!MclaSaveInProgress()) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
          return "error no save began within 10 s";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    return MclaSaveInProgress() ? "ok saving" : "ok idle";
  }
  if (cmd == "close") {
#if defined(_WIN32)
    // The close button's WM_CLOSE, to each of this process's windows.
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
          DWORD pid = 0;
          GetWindowThreadProcessId(hwnd, &pid);
          if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
          }
          return TRUE;
        },
        0);
    return "ok";
#else
    return "error close is for Windows; use quit";
#endif
  }
  if (cmd == "poke") {
    // poke <addr> <u32>: writes one big-endian word, for experiments such as
    // changing a game setting where the game keeps it.
    uint32_t address;
    double value;
    if (!EvalAddress(arg(1), address) || !ParseNumber(arg(2), value)) {
      return "err poke <addr> <u32>";
    }
    uint8_t* p = const_cast<uint8_t*>(GuestPointer(address, 4));
    if (!p) {
      return "err unwritable";
    }
    const uint32_t word = uint32_t(int64_t(value));
    p[0] = uint8_t(word >> 24);
    p[1] = uint8_t(word >> 16);
    p[2] = uint8_t(word >> 8);
    p[3] = uint8_t(word);
    char text[48];
    std::snprintf(text, sizeof text, "ok %08X = %08X", address, word);
    return text;
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
  if (cmd == "screen") {
#if defined(_WIN32)
    double scale = 1.0;
    size_t last = args.size() - 1;
    if (args.size() > 2 && ParseNumber(args[last], scale)) {
      last--;
    }
    std::string result;
    return ScreenShot(rest(1, last), scale, result) ? "ok " + result : "err " + result;
#else
    return "err screen is for Windows";
#endif
  }
  if (cmd == "window") {
#if defined(_WIN32)
    return WindowCommand(arg(1));
#else
    return "err window is for Windows";
#endif
  }
  if (cmd == "state") {
    const uint64_t elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_start).count());
    return "ok frame=" + std::to_string(g_frame.load()) + " swaps=" +
           std::to_string(g_swaps.load()) + " ms=" + std::to_string(elapsed) +
           " foreign_frame_calls=" + std::to_string(g_foreign_frame_calls.load()) +
           " pump_rounds=" + std::to_string(MclaPumpRounds()) +
           " playing=" + std::to_string(g_playing.load()) +
           " state_polls=" + std::to_string(g_state_polls.load()) +
           " key_polls=" + std::to_string(g_key_polls.load()) +
           " keys_sent=" + std::to_string(g_keys_sent.load()) + " " + FormatPad(GetPad());
  }
  if (cmd == "set") {
    // An option changed while running, for A/B checks within one run.
    if (args.size() != 3) {
      return "err usage: set <option> <value>";
    }
    if (!rex::cvar::SetFlagByName(args[1], args[2])) {
      return "err cannot set " + args[1];
    }
    return "ok " + args[1] + "=" + rex::cvar::GetFlagByName(args[1]);
  }
  if (cmd == "get") {
    // The options' values, in the reply and in the log (a script has no reply to read).
    if (args.size() < 2) {
      return "err usage: get <option>...";
    }
    std::string values;
    for (size_t i = 1; i < args.size(); ++i) {
      std::string value;
      if (args[i] == "fg_status") {
        const auto status = rex::ui::Presenter::GetFrameGenerationStatus();
        value = status.state == rex::ui::Presenter::FrameGenerationState::kUnknown ? "unknown"
                : status.state == rex::ui::Presenter::FrameGenerationState::kAvailable
                    ? "available"
                    : "unavailable (" + status.why + ")";
      } else if (args[i] == "ll_status") {
        const auto status = rex::ui::LowLatency::GetStatus();
        value = status.state == rex::ui::LowLatency::State::kUnknown ? "unknown"
                : status.state == rex::ui::LowLatency::State::kAvailable
                    ? std::string(status.active ? "active" : "idle") + " (" + status.name + ")"
                    : "unavailable (" + status.name + ": " + status.why + ")";
      } else {
        value = rex::cvar::GetFlagByName(args[i]);
      }
      values += (i > 1 ? " " : "") + args[i] + "=" + value;
    }
    REXLOG_INFO("dev pad: get {}", values);
    return "ok " + values;
  }
  if (cmd == "save_config") {
    // The F4 overlay's Save button (src/ui/overlay/settings_overlay.cpp), for checking what
    // it writes.
    rex::cvar::SaveConfig(rex::filesystem::GetExecutableFolder() / "mcla.toml");
    return "ok";
  }
  if (cmd == "key") {
    // key <name>: a key press for the window's key binds (F3 the debug
    // overlay, F7 the achievements), for checking the host UI over the game.
    const rex::ui::VirtualKey key = args.size() == 2 ? rex::ui::ParseVirtualKey(args[1])
                                                     : rex::ui::VirtualKey::kNone;
    if (key == rex::ui::VirtualKey::kNone || !g_window) {
      return "err usage: key <name>";
    }
    rex::ui::Window* window = g_window;
    window->app_context().CallInUIThread([window, key] {
      rex::ui::KeyEvent e(window, key, 0, false, false, false, false, false);
      rex::ui::ProcessKeyEvent(e);
    });
    return "ok";
  }
  if (cmd == "testdialog") {
    double seconds = 3.0;
    if (args.size() == 2 && !ParseNumber(args[1], seconds)) {
      return "err usage: testdialog [seconds]";
    }
    if (!g_window || !g_imgui_drawer) {
      return "err no window";
    }
    rex::ui::ImGuiDrawer* drawer = g_imgui_drawer;
    g_window->app_context().CallInUIThread([drawer, seconds] {
      // Owned by the drawer; it deletes itself when it closes.
      new SelfClosingTestDialog(drawer, seconds);
    });
    return "ok";
  }
  if (cmd == "write_content_header") {
    auto* runtime = rex::Runtime::instance();
    auto* kernel = runtime ? runtime->kernel_state() : nullptr;
    if (!kernel || !kernel->content_manager() || args.size() != 2) {
      return "err usage: write_content_header <name> (needs a running game)";
    }
    rex::system::xam::XCONTENT_AGGREGATE_DATA data;
    std::memset(&data, 0xEE, sizeof(data));
    data.device_id = 1;
    data.content_type = rex::system::XContentType::kSavedGame;
    data.set_display_name(u"HEADER TEST");
    data.set_file_name(args[1]);
    data.xuid = 0;
    data.title_id = rex::system::xam::kCurrentlyRunningTitleId;
    const auto result = kernel->content_manager()->WriteContentHeaderFile(0, data);
    return result == X_ERROR_SUCCESS ? "ok" : "err " + std::to_string(uint32_t(result));
  }
  if (cmd == "toast") {
    // toast [id]: the achievement pop-up for an achievement (the first one
    // listed by default), as at an unlock, without unlocking or saving anything
    // (AchievementManager::ShowAchievementNotification), for checking the
    // presenter while a pop-up is shown.
    auto* runtime = rex::Runtime::instance();
    auto* kernel = runtime ? runtime->kernel_state() : nullptr;
    if (!kernel) {
      return "err no kernel state";
    }
    auto& achievements = kernel->achievements();
    uint32_t id = 0;
    if (args.size() == 2) {
      id = uint32_t(std::strtoul(args[1].c_str(), nullptr, 0));
    } else {
      const auto all = achievements.ListAchievements();
      if (all.empty()) {
        return "err no achievements";
      }
      id = all.front().id;
    }
    return achievements.ShowAchievementNotification(id) ? "ok " + std::to_string(id)
                                                        : "err no achievement " + std::to_string(id);
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
  if (cmd == "cars") {
    // Every race car: address and live position; the player first.
    const auto cars = FindCars();
    std::string reply = "ok frame=" + std::to_string(g_frame.load()) + " cars=" +
                        std::to_string(cars.size());
    for (const uint32_t car : cars) {
      float xyz[3] = {};
      for (int i = 0; i < 3; i++) {
        ReadFloat(car + kCarLivePositionOffset + 4 * i, xyz[i]);
      }
      char text[96];
      std::snprintf(text, sizeof text, " | %08X %.2f %.2f %.2f", car, xyz[0], xyz[1], xyz[2]);
      reply += text;
    }
    return reply;
  }
  if (cmd == "watch") {
    // watch <every updates> | watch stop
    double every = 0;
    if (arg(1) != "stop" && (!ParseNumber(arg(1), every) || every < 1)) {
      return "err watch <every updates> | watch stop";
    }
    std::lock_guard lock(g_watch_mutex);
    g_watch_cars = arg(1) == "stop" ? std::vector<uint32_t>{} : FindCars();
    g_watch_every = static_cast<int>(every);
    g_watch_last = -1;
    g_watching = g_watch_every > 0;
    return "ok cars=" + std::to_string(g_watch_cars.size());
  }
  if (cmd == "screens") {
    std::string reply = "ok";
    for (const auto& name : UiScreens()) {
      reply += " " + name;
    }
    return reply;
  }
  if (cmd == "follow") {
    // follow <route> [speed=1] | follow stop | follow status
    if (arg(1) == "stop" || arg(1) == "status") {
      std::string summary;
      {
        std::lock_guard lock(g_follow_mutex);
        summary = g_follow.route ? FollowSummary(g_follow) : "not following";
      }
      if (arg(1) == "stop") {
        StopFollowing("stopped");
      }
      return "ok " + summary;
    }
    double scale = 1.0;
    size_t last = args.size() - 1;
    if (args.size() > 2 && args[last].rfind("speed=", 0) == 0) {
      if (!ParseNumber(args[last].substr(6), scale)) {
        return "err speed";
      }
      last--;
    }
    std::string error;
    auto route = LoadRoute(ScriptRelative(rest(1, last)), error);
    if (!route) {
      return "err " + error;
    }
    Stillness tips;
    StartStillness(tips);  // the heap scan, before the lock
    std::lock_guard lock(g_follow_mutex);
    g_follow = {};
    g_follow.route = route;
    g_follow.speed_scale = scale;
    g_follow.tips = std::move(tips);
    g_following = true;
    return "ok " + std::to_string(route->points.size()) + " points, " +
           std::to_string(g_follow.tips.cars.size()) + " cars";
  }
  if (cmd == "findvec") {
    // findvec <addr> [<floats>=3] [<tolerance>=0.02]: where else in guest memory the same floats
    // are, each within the tolerance (with the world frozen, `set mcla_freeze true`, so that they
    // are not changing meanwhile). For finding the copies of a car's position, to move it for real.
    uint32_t address;
    double count = 3, tolerance = 0.02;
    if (!EvalAddress(arg(1), address) || (args.size() > 2 && !ParseNumber(arg(2), count)) ||
        (args.size() > 3 && !ParseNumber(arg(3), tolerance)) || count < 1 || count > 16) {
      return "err findvec <addr> [floats] [tolerance]";
    }
    float want[16];
    for (size_t k = 0; k < size_t(count); k++) {
      if (!ReadFloat(address + 4 * uint32_t(k), want[k])) {
        return "err unreadable";
      }
    }
    const auto hits = FindFloats(want, size_t(count), float(tolerance));
    uint32_t vehicle = 0;
    ReadU32(kPlayerVehicle, vehicle);
    std::string reply = "ok " + std::to_string(hits.size()) + " matches:";
    for (size_t i = 0; i < hits.size() && i < 64; i++) {
      char text[64];
      if (vehicle && hits[i] >= vehicle && hits[i] < vehicle + 0x2000) {
        std::snprintf(text, sizeof text, " %08X(vehicle+0x%X)", hits[i], hits[i] - vehicle);
      } else {
        std::snprintf(text, sizeof text, " %08X", hits[i]);
      }
      reply += text;
      if (i < 6 && !(vehicle && hits[i] >= vehicle && hits[i] < vehicle + 0x2000)) {
        // What is around a match elsewhere: 24 floats from 0x30 before it.
        reply += " {";
        for (int k = -12; k < 12; k++) {
          float f = 0;
          ReadFloat(hits[i] + uint32_t(4 * k), f);
          std::snprintf(text, sizeof text, "%g%s", f, k == 11 ? "" : " ");
          reply += text;
        }
        reply += "}";
      }
    }
    return reply;
  }
  if (cmd == "autocontinue") {
    // autocontinue on | off | status: A for a dialog that stops the world (a tip over the race:
    // "CONTINUE (A)"), for a script that doesn't follow a route (the follower does it itself).
    if (arg(1) == "on") {
      StartStillness(g_tips);
      g_tips_a_down = false;
      g_autocontinue = true;
      return "ok " + std::to_string(g_tips.cars.size()) + " cars";
    }
    if (arg(1) == "off") {
      g_autocontinue = false;
      if (g_tips_a_down) {
        g_tips_a_down = false;
        EditPad([&](PadValues& v) { v.buttons &= ~0x1000; });
      }
    } else if (arg(1) != "status") {
      return "err autocontinue on | off | status";
    }
    REXLOG_INFO("dev pad: autocontinue: {} pressed of A for a dialog", g_tips.dismissed);
    return "ok dismissed=" + std::to_string(g_tips.dismissed) + (g_autocontinue ? " on" : " off");
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

std::string RunScriptFile(const std::string& name) {
  const std::string path = ScriptRelative(name);
  std::ifstream in(path);
  if (!in) {
    return "err cannot read " + path;
  }
  const std::string saved_dir = g_script_dir;
  g_script_dir = path.substr(0, path.find_last_of("\\/"));
  int number = 0;
  for (std::string line; std::getline(in, line);) {
    number++;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::string reply = Execute(line);
    if (reply.rfind("err", 0) == 0) {
      // The rest would run out of step (a menu not reached, a race not
      // started), so the script stops here.
      REXLOG_WARN("dev pad: {}:{}: {} -> {}", path, number, line, reply);
      g_script_dir = saved_dir;
      return reply.rfind("err at ", 0) == 0
                 ? reply
                 : "err at " + path + ":" + std::to_string(number) + ": " + line + " -> " + reply;
    }
    if (line.find_first_not_of(" \t") != std::string::npos &&
        line[line.find_first_not_of(" \t")] != '#') {
      REXLOG_INFO("dev pad: {} -> {}", line, reply);
    }
  }
  g_script_dir = saved_dir;
  return "ok";
}

void RunScript(std::string path) {
  REXLOG_INFO("dev pad: running script {}", path);
  const std::string reply = RunScriptFile(path);
  if (reply == "ok") {
    REXLOG_INFO("dev pad: script {} done: {}", path, reply);
  } else {
    REXLOG_ERROR("dev pad: script {} done: {}", path, reply);
  }
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
void MclaDevPadStart(rex::ui::Window* window, rex::ui::ImGuiDrawer* imgui_drawer) {
  if (!g_enabled) {
    return;
  }
  g_window = window;
  g_imgui_drawer = imgui_drawer;
  g_start = Clock::now();
  if (const std::string path = REXCVAR_GET(mcla_pad_frames_csv); !path.empty()) {
    g_frames_csv = std::fopen(path.c_str(), "w");
    if (g_frames_csv) {
      std::setvbuf(g_frames_csv, nullptr, _IOFBF, 1 << 20);
      std::fprintf(g_frames_csv, "kind,a,b,c\n");
      // The start on the steady clock's own scale (ns; on Windows the
      // performance counter, as the SDK's present_log and PresentMon's
      // --qpc_time), so swaps can be lined up with presents.
      std::fprintf(g_frames_csv, "t0,%lld,0,0\n",
                   static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              g_start.time_since_epoch())
                                              .count()));
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
  if (REXCVAR_GET(mcla_freeze)) {
    ticks = 1;
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
  {
    // The frame's start (after the low-latency wait, which comes first in the timer), and the
    // simulation step the engine takes (microseconds), for presents.py's start to screen time
    // and the step against the frames' display times.
    std::lock_guard lock(g_log_mutex);
    if (g_frames_csv) {
      const auto since_start =
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - g_start).count();
      const uint64_t step_us = ticks * 1'000'000ull / rex::chrono::Clock::guest_tick_frequency();
      std::fprintf(g_frames_csv, "f,%lld,%llu,%llu\n", static_cast<long long>(since_start),
                   static_cast<unsigned long long>(step_us),
                   static_cast<unsigned long long>(frame));
    }
  }
  if (g_playing.load(std::memory_order_relaxed)) {
    PlayFrame(frame);
  }
  if (g_following.load(std::memory_order_relaxed)) {
    FollowFrame(static_cast<double>(ticks) /
                static_cast<double>(rex::chrono::Clock::guest_tick_frequency()));
  } else if (g_autocontinue.load(std::memory_order_relaxed)) {
    AutoContinueFrame();
  }
  if (g_watching.load(std::memory_order_relaxed)) {
    WatchFrame(frame);
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

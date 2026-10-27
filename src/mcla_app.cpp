#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>
#include <shellapi.h>
#endif

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xfile.h>

REXCVAR_DEFINE_STRING(mcla_gpu_backend, "any", "MCLA",
                      "Graphics backend for the Xenos GPU plugin: any, vulkan or d3d12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(mcla_hide_cursor, true, "MCLA",
                    "Hide the mouse pointer over the game when a controller is used or the "
                    "mouse has been still for a few seconds; moving the mouse or clicking "
                    "shows it again");


std::unique_ptr<rex::system::IAudioSystem> CreateMclaAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher);
void MclaDevPadConfigure(rex::RuntimeConfig& config);
void MclaDevPadStart(rex::ui::Window* window, rex::ui::ImGuiDrawer* imgui_drawer);
void MclaSaveMenuSettings();  // src/settings_menu.cpp
bool MclaPrepareShadersIsRequested();  // src/shader_prepare.cpp
void MclaShaderStorageAfterPreload(rex::Runtime* runtime);
void MclaMigrateAntiAliasing();  // src/perf_options.cpp
void MclaApplyPreset();  // src/perf_options.cpp
#ifdef _WIN32
static bool AutomaticBackendIsVulkan();
#endif

#ifdef _WIN32
// The pause menu's RESTART GAME (src/settings_menu.cpp) starts the game again
// with this on its command line: a handle to the old process, inherited by the
// new one (a handle, not a process id, which could be reused). The new one
// waits for the old one to be gone before it reads the settings, opens its
// window or loads the GPU plugin; if it is still there after the wait, the new
// one quits rather than run beside it, as two games would both write the
// career save. Read straight from the command line, not as an option, so that
// it can never be saved into mcla.toml.
constexpr std::wstring_view kRestartFlag = L"--mcla_restart_wait_handle=";

static void WaitForRestartedGame() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return;
  }
  HANDLE old_game = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::wstring_view(argv[i]).starts_with(kRestartFlag)) {
      old_game = reinterpret_cast<HANDLE>(
          static_cast<uintptr_t>(std::wcstoull(argv[i] + kRestartFlag.size(), nullptr, 16)));
    }
  }
  LocalFree(argv);
  if (!old_game) {
    return;
  }
  const DWORD waited = WaitForSingleObject(old_game, 30'000);
  CloseHandle(old_game);
  if (waited == WAIT_TIMEOUT) {
    MessageBoxW(nullptr,
                L"The game that was restarting is still running, so this one is not starting. "
                L"Close it, then start the game again.",
                L"Midnight Club: Los Angeles", MB_OK | MB_ICONWARNING);
    ExitProcess(1);
  }
}

// One argument quoted as CommandLineToArgvW reads it back.
static std::wstring QuoteArgument(std::wstring_view arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(arg);
  }
  std::wstring out = L"\"";
  size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    // Backslashes are literal unless they precede a quote.
    out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
    backslashes = 0;
    out += c;
  }
  out.append(backslashes * 2, L'\\');
  return out + L'"';
}

// The pause menu's RESTART GAME: starts mcla.exe again with this process's
// command line, plus an inheritable handle to this process for the new one to
// wait on (WaitForRestartedGame). Only that handle is inherited. The caller
// saves the settings first and closes the game after.
bool MclaRelaunchGame() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return false;
  }
  std::wstring command;
  for (int i = 0; i < argc; ++i) {
    if (i > 0 && std::wstring_view(argv[i]).starts_with(kRestartFlag)) {
      continue;
    }
    command += (command.empty() ? L"" : L" ") + QuoteArgument(argv[i]);
  }
  LocalFree(argv);

  HANDLE self = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
                       SYNCHRONIZE, TRUE, 0)) {
    REXLOG_ERROR("restart: no handle for the new game to wait on (error {})", GetLastError());
    return false;
  }
  wchar_t handle_text[32];
  std::swprintf(handle_text, 32, L"%llx", static_cast<unsigned long long>(
                                              reinterpret_cast<uintptr_t>(self)));
  command += L" " + std::wstring(kRestartFlag) + handle_text;

  SIZE_T list_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &list_size);
  std::vector<uint8_t> list_storage(list_size);
  auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(list_storage.data());
  std::wstring exe(32768, L'\0');
  exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const bool started =
      !exe.empty() && InitializeProcThreadAttributeList(list, 1, 0, &list_size) &&
      UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &self, sizeof(self),
                                nullptr, nullptr) &&
      (startup.lpAttributeList = list,
       CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                      EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo,
                      &process));
  const DWORD error = GetLastError();
  if (startup.lpAttributeList) {
    DeleteProcThreadAttributeList(list);
  }
  CloseHandle(self);
  if (!started) {
    REXLOG_ERROR("restart: could not start the game again (error {})", error);
    return false;
  }
  REXLOG_INFO("restart: started again as process {}", process.dwProcessId);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}
#endif

// A career save being written: a file open for writing on a content package,
// which the SDK mounts at \Device\Content\<n>\ (xam/content_manager.cpp).
static bool SaveInProgress() {
  auto* runtime = rex::Runtime::instance();
  auto* kernel = runtime ? runtime->kernel_state() : nullptr;
  if (!kernel) {
    return false;
  }
  // GENERIC_WRITE, GENERIC_ALL, FILE_WRITE_DATA, FILE_APPEND_DATA.
  constexpr uint32_t kWriteAccess = 0x40000000u | 0x10000000u | 0x2u | 0x4u;
  for (const auto& file : kernel->object_table()->GetObjectsByType<rex::system::XFile>()) {
    if (file && (file->file_access() & kWriteAccess) &&
        file->device()->mount_path().starts_with("\\Device\\Content\\")) {
      return true;
    }
  }
  return false;
}

bool MclaSaveInProgress() {  // for the dev pad's `saving`
  return SaveInProgress();
}

// Before the game ends (QUIT GAME, RESTART GAME, the window's close button):
// a career save that is being written is let finish first, for up to ten
// seconds. Ending the title gives the game's threads only 200 ms, which could
// cut a save short. A save takes well under a second (50 ms measured), and
// the log says when closing waited for one.
void MclaWaitForSaves() {
  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  if (!SaveInProgress()) {
    return;
  }
  REXLOG_INFO("closing: a save is being written; waiting for it");
  while (SaveInProgress() && Clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const auto waited =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  if (SaveInProgress()) {
    REXLOG_WARN("closing: the save was still being written after {} ms", waited);
  } else {
    REXLOG_INFO("closing: the save finished after {} ms", waited);
  }
}

namespace {

// A game kept where it is (the setup's "Use it where it is") is named by
// game_data_root in mcla.toml. The SDK reads that file after it has decided the
// paths, so the line is read here; a value on the command line wins over it.
// A relative value is relative to the folder of mcla.toml, not to wherever the
// game was started from. The folder is returned whether or not it exists: a
// folder that is gone must be said (ConstructRuntime below), not replaced by
// another one. Empty when the file or the line is not there (or the file is not
// valid TOML), which leaves everything as it was.
std::filesystem::path GameDataRootFromConfig(const std::filesystem::path& config_path) {
  std::error_code error;
  if (config_path.empty() || !std::filesystem::exists(config_path, error)) {
    return {};
  }
  try {
    std::ifstream stream(config_path, std::ios::binary);
    const auto table = toml::parse(stream);
    if (const auto root = table["game_data_root"].value<std::string>(); root && !root->empty()) {
      std::filesystem::path folder = std::filesystem::u8path(*root);
      if (folder.is_relative()) {
        folder = std::filesystem::absolute(config_path.parent_path() / folder, error);
      }
      return folder.lexically_normal();
    }
  } catch (const toml::parse_error&) {
  }
  return {};
}

// What the game needs from its folder: the program and the archives. Names of the
// 13 files of the one supported release (scripts/ci/game-files.json); the others
// (videos, art) are not checked.
constexpr const char* kRequiredGameFiles[] = {
    "default.xex", "xarchive_audio.rpf", "xarchive_audlo.rpf", "xarchive_cache.rpf", "xarchive_music.rpf"};

}  // namespace

// Started with no options (a double-click on mcla.exe), the game should just
// run: use the folder mcla.toml names, else find the extracted game next to the
// executable or, as the build leaves it, in the repository a few folders up,
// and load the Xenos GPU plugin.
void MclaApp::OnConfigurePaths(rex::PathConfig& paths) {
#ifdef _WIN32
  WaitForRestartedGame();
#endif
  if (!paths.game_data_root.empty()) {
    return;
  }
  paths.game_data_root = GameDataRootFromConfig(paths.config_path);
  if (!paths.game_data_root.empty()) {
    configured_game_root_ = paths.game_data_root;
    configured_game_root_file_ = paths.config_path;
    return;
  }
  std::filesystem::path folder = rex::filesystem::GetExecutableFolder();
  for (int level = 0; level < 5 && !folder.empty(); ++level) {
    std::error_code error;
    if (std::filesystem::exists(folder / "game" / "default.xex", error)) {
      paths.game_data_root = folder / "game";
      return;
    }
    if (folder == folder.parent_path()) {
      break;
    }
    folder = folder.parent_path();
  }
}

bool MclaApp::ConstructRuntime(const rex::PathConfig& paths) {
  if (!configured_game_root_.empty() && paths.game_data_root == configured_game_root_) {
    std::error_code error;
    std::string problem;
    if (!std::filesystem::is_directory(configured_game_root_, error)) {
      problem = "That folder is not there.";
    } else {
      for (const char* name : kRequiredGameFiles) {
        if (!std::filesystem::exists(configured_game_root_ / name, error)) {
          problem += (problem.empty() ? "These files are missing from it: " : ", ");
          problem += name;
        }
      }
      if (!problem.empty()) {
        problem += ".";
      }
    }
    if (!problem.empty()) {
      const std::string message = fmt::format(
          "The game's files are not where {} says they are.\n\n{}\n{}\n\nPut the game back there, or change game_data_root "
          "in that file to where it is now (or run the setup again on this install folder to choose where the game is).",
          configured_game_root_file_.string(), configured_game_root_.string(), problem);
      REXLOG_ERROR("{}", message);
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error, message);
      return false;
    }
  }
  return rex::ReXApp::ConstructRuntime(paths);
}

void MclaApp::OnPreSetup(rex::RuntimeConfig& config) {
  config.audio_factory = &CreateMclaAudioSystem;
  // Every controller is player one. The SDK's default gives each pad its own
  // player in connection order and never reuses a number, so a second gamepad
  // device (Steam Input, DS4Windows, a wheel) or a pad that reconnects becomes
  // player two, which has no profile: the game then says it is not signed in
  // and never saves (issue #22). The game is single-player on one console.
  config.input_factory = [](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto input = rex::input::CreateDefaultInputSystem(tool_mode);
    if (input) {
      input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
    }
    return input;
  };
  // Preparing shaders shows nothing: a window, not the full screen, hidden once the runtime is up.
  // A command-line value, which mcla.toml never saves.
  if (MclaPrepareShadersIsRequested()) {
    rex::cvar::SetFlagFromCommandLine("fullscreen", "false");
  }
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "xenos";
  }

  // Loaded here, also for "any" (the plugin named by --gpu_plugin, with its own
  // default backend, as the SDK would load it), so that the PC defaults reach
  // the GPU's options before the GPU starts. A backend that fails to load falls
  // back to the plugin's default, as the SDK would do next; if that fails too,
  // the SDK tries once more and reports it.
  std::string backend = REXCVAR_GET(mcla_gpu_backend);
#ifdef _WIN32
  if (backend == "any" && AutomaticBackendIsVulkan()) {
    backend = "vulkan";
  }
#endif
  if (backend != "any") {
    config.graphics = rex::system::LoadGpuPlugin("xenos", backend);
    if (!config.graphics) {
      REXLOG_ERROR("could not load the xenos GPU plugin with backend '{}'; trying its default",
                   backend);
    }
  }
  if (!config.graphics) {
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
  }
  ApplyPcDefaults();
  MclaMigrateAntiAliasing();
  MclaApplyPreset();

  // Development: scripted controller input (src/dev_pad.cpp), off by default.
  MclaDevPadConfigure(config);
}

#ifdef _WIN32
// What "any" (Automatic) means on Windows: Vulkan on AMD and Intel graphics, otherwise the
// plugin's own choice, Direct3D 12. The GPU is the one Direct3D 12 would take (the SDK's
// d3d12_adapter: the first physical adapter unless one is given), which Vulkan's loader
// lists in the same order. Measured on an AMD Ryzen 9800X3D's integrated Radeon: Vulkan is
// 6 to 10% faster at the game's own resolution (launcher/dev/gpu-weak-2026-10-27.md). On
// Intel the SDK takes a slower path of rasterizer-ordered views for render targets on
// Direct3D 12, where Vulkan uses ordinary framebuffer ones (not measured: no Intel GPU was
// available). NVIDIA stays on Direct3D 12: its Vulkan is not faster in races. Without a
// Vulkan driver (no vulkan-1.dll) it stays on Direct3D 12 too. Choosing the API in the
// launcher or the pause menu, or --mcla_gpu_backend, skips all this.
static bool AutomaticBackendIsVulkan() {
  const int adapter_option = std::atoi(rex::cvar::GetFlagByName("d3d12_adapter").c_str());
  if (adapter_option == -2) {
    return false;  // WARP.
  }
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
    return false;
  }
  UINT vendor = 0;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    const bool described = SUCCEEDED(adapter->GetDesc1(&desc));
    adapter->Release();
    if (!described) {
      continue;
    }
    if (adapter_option >= 0 ? int(i) == adapter_option : !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
      vendor = desc.VendorId;
      break;
    }
  }
  factory->Release();
  if (vendor != 0x1002 && vendor != 0x8086) {
    return false;
  }
  HMODULE vulkan = LoadLibraryW(L"vulkan-1.dll");
  if (!vulkan) {
    return false;
  }
  FreeLibrary(vulkan);
  REXLOG_INFO("graphics API: Vulkan (Automatic on {} graphics)", vendor == 0x1002 ? "AMD" : "Intel");
  return true;
}
#endif

// SDK settings measured to suit this game on PC (README.md, "Turning a change
// off"; the PS5 host sets its own). They become the options' defaults
// (rex::cvar::SetDefaultValue): used unless the command line, an environment
// variable or mcla.toml sets them, and never saved as if the user had chosen
// them. Most were options the Windows launcher passed. Set once the GPU
// plugin, which defines them, is loaded, and before the GPU starts, which
// reads edram_tiles.
void MclaApp::ApplyPcDefaults() {
  // - uploaded guest memory stays valid across frames instead of being copied
  //   to the GPU again every frame;
  // - vertex shaders of draws with clipping off run on the CPU, as in Xenia,
  //   so they no longer claim the whole EDRAM;
  // - hot pages, as on the PS5: guest memory the game rewrites all the time is
  //   uploaded when drawn from instead of being write-protected again after
  //   every fault;
  // - an EDRAM of 4,096 tiles, so that the scene fits in one tile with 2x MSAA
  //   (mcla_single_tile, src/render_perf.cpp);
  // - on Windows, HDR output whenever the display is in HDR mode (the SDK's
  //   own default stays off, for other titles). Not on Linux, where the
  //   display's HDR state isn't read yet, so auto would never turn it on
  //   (on would), nor on the PS5, whose host doesn't apply these.
  constexpr std::pair<const char*, const char*> kDefaults[] = {
      {"clear_memory_page_state", "false"},
      {"execute_unclipped_draw_vs_on_cpu", "true"},
      {"shared_memory_hot_page_faults", "4"},
      {"shared_memory_hot_page_ms", "10000"},
      {"edram_tiles", "4096"},
#if REX_PLATFORM_WIN32
      {"hdr", "auto"},
#endif
  };
  for (const auto& [name, value] : kDefaults) {
    const bool user_set = rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault;
    if (!rex::cvar::SetDefaultValue(name, value)) {
      REXLOG_WARN("PC defaults: {} = {} not taken", name, value);
    } else if (user_set) {
      REXLOG_INFO("PC defaults: {} = {} set by the user", name, rex::cvar::GetFlagByName(name));
    } else {
      REXLOG_INFO("PC defaults: {} = {}", name, value);
    }
  }
}

// As PC games do with a controller: the pointer goes away during play and
// comes back with the mouse. Only over the game while it has focus; the SDK's
// mouse-driven overlays keep it shown. Follows the option while the game runs
// (the pause menu's HIDE MOUSE POINTER).
void MclaApp::FollowCursorSetting() {
  auto apply = [this](bool hide) {
    if (window()) {
      window()->SetCursorVisibility(hide ? rex::ui::Window::CursorVisibility::kAutoHidden
                                         : rex::ui::Window::CursorVisibility::kVisible);
    }
  };
  if (REXCVAR_GET(mcla_hide_cursor)) {
    apply(true);
  }
  // Runs where the option is set, under the option registry's lock; the
  // window is changed on the UI thread.
  rex::cvar::RegisterChangeCallback("mcla_hide_cursor",
                                    [this, apply](std::string_view, std::string_view value) {
                                      const bool hide = value == "true";
                                      REXLOG_INFO("mouse pointer: {}",
                                                  hide ? "hidden during play" : "always shown");
                                      app_context().CallInUIThread([apply, hide] { apply(hide); });
                                    });
}

void MclaApp::OnPostLaunchModule(rex::system::XThread* thread) {
  (void)thread;
  MclaShaderStorageAfterPreload(runtime());
}

bool MclaApp::OnWindowCloseRequested() {
  MclaWaitForSaves();
  MclaSaveMenuSettings();
  return true;
}

void MclaApp::OnPostSetup() {
  ConfigureFrameTiming();
  FollowCursorSetting();

  MclaDevPadStart(window(), imgui_drawer());

  // The game probes for loose city data on a "t:" drive that retail consoles
  // do not have. Give it a device so the probes fail with "not found" rather
  // than "no such device". LARecomp does the same.
  if (runtime() && runtime()->file_system()) {
    runtime()->file_system()->RegisterSymbolicLink("t:", "\\Device\\Harddisk0\\Partition1");
  }
}

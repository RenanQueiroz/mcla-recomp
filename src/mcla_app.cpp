#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>

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
void MclaDevPadStart(rex::ui::Window* window);
void MclaSaveMenuSettings();  // src/settings_menu.cpp

#ifdef _WIN32
// The pause menu's RESTART GAME (src/settings_menu.cpp) starts the game again
// with this added to its command line, and quits. The new one waits for the
// old one to be gone before it reads the settings, opens its window or loads
// the GPU plugin. Read straight from the command line, not as an option, so
// it can never be saved into mcla.toml.
static void WaitForRestartedGame() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return;
  }
  constexpr std::wstring_view kFlag = L"--mcla_restart_wait_pid=";
  DWORD pid = 0;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view arg = argv[i];
    if (arg.starts_with(kFlag)) {
      pid = DWORD(std::wcstoul(argv[i] + kFlag.size(), nullptr, 10));
    }
  }
  LocalFree(argv);
  if (!pid) {
    return;
  }
  if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
    WaitForSingleObject(process, 30'000);
    CloseHandle(process);
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
// command line, plus the id the new one waits on (WaitForRestartedGame). The
// caller saves the settings first and quits after.
bool MclaRelaunchGame() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) {
    return false;
  }
  std::wstring command;
  for (int i = 0; i < argc; ++i) {
    if (i > 0 && std::wstring_view(argv[i]).starts_with(L"--mcla_restart_wait_pid=")) {
      continue;
    }
    command += (command.empty() ? L"" : L" ") + QuoteArgument(argv[i]);
  }
  LocalFree(argv);
  command += L" --mcla_restart_wait_pid=" + std::to_wstring(GetCurrentProcessId());

  std::wstring exe(32768, L'\0');
  exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (exe.empty() || !CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                                     nullptr, nullptr, &startup, &process)) {
    REXLOG_ERROR("restart: could not start the game again (error {})", GetLastError());
    return false;
  }
  REXLOG_INFO("restart: started again as process {}", process.dwProcessId);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}
#endif

// Started with no options (a double-click on mcla.exe), the game should just
// run: find the extracted game next to the executable or, as the build leaves
// it, in the repository a few folders up, and load the Xenos GPU plugin.
void MclaApp::OnConfigurePaths(rex::PathConfig& paths) {
#ifdef _WIN32
  WaitForRestartedGame();
#endif
  if (!paths.game_data_root.empty()) {
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

void MclaApp::OnPreSetup(rex::RuntimeConfig& config) {
  config.audio_factory = &CreateMclaAudioSystem;
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "xenos";
  }

  // Left to the SDK when "any"; it then loads the plugin named by --gpu_plugin
  // with its own default backend.
  std::string backend = REXCVAR_GET(mcla_gpu_backend);
  if (backend != "any") {
    config.graphics = rex::system::LoadGpuPlugin("xenos", backend);
    if (!config.graphics) {
      REXLOG_ERROR("could not load the xenos GPU plugin with backend '{}'", backend);
    }
  }

  // Development: scripted controller input (src/dev_pad.cpp), off by default.
  MclaDevPadConfigure(config);
}

// SDK settings measured to suit this game on PC (README.md, "Turning a change
// off"; the PS5 host sets its own). Each is used unless the command line, an
// environment variable or mcla.toml already set it, and none of them is
// written to mcla.toml. They were options the Windows launcher passed.
void MclaApp::ApplyPcDefaults() {
  // - uploaded guest memory stays valid across frames instead of being copied
  //   to the GPU again every frame;
  // - vertex shaders of draws with clipping off run on the CPU, as in Xenia,
  //   so they no longer claim the whole EDRAM;
  // - hot pages, as on the PS5: guest memory the game rewrites all the time is
  //   uploaded when drawn from instead of being write-protected again after
  //   every fault.
  constexpr std::pair<const char*, const char*> kDefaults[] = {
      {"clear_memory_page_state", "false"},
      {"execute_unclipped_draw_vs_on_cpu", "true"},
      {"shared_memory_hot_page_faults", "4"},
      {"shared_memory_hot_page_ms", "10000"},
  };
  for (const auto& [name, value] : kDefaults) {
    if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) {
      REXLOG_INFO("PC defaults: {} = {} set by the user", name, rex::cvar::GetFlagByName(name));
    } else if (rex::cvar::SetFlagByName(name, value)) {
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

bool MclaApp::OnWindowCloseRequested() {
  MclaSaveMenuSettings();
  return true;
}

void MclaApp::OnPostSetup() {
  ConfigureFrameTiming();
  ApplyPcDefaults();
  FollowCursorSetting();

  MclaDevPadStart(window());

  // The game probes for loose city data on a "t:" drive that retail consoles
  // do not have. Give it a device so the probes fail with "not found" rather
  // than "no such device". LARecomp does the same.
  if (runtime() && runtime()->file_system()) {
    runtime()->file_system()->RegisterSymbolicLink("t:", "\\Device\\Harddisk0\\Partition1");
  }
}

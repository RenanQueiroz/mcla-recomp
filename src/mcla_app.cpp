#include "generated/default/mcla_init.h"

#include "mcla_app.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi.h>
#include <shellapi.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
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

// Lets a career save in progress finish before the game ends.
void MclaWaitForSaves();  // src/save_wait.cpp

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

#ifdef _WIN32
// --- Automatic graphics API (mcla_gpu_backend = any) -----------------------------------
//
// Vulkan on AMD's integrated GPUs (the ones that share the CPU's memory) and on Intel's;
// Direct3D 12 on AMD's discrete cards, on NVIDIA and when nothing is known. Which GPU: the
// one Direct3D 12 would take (the SDK's d3d12_adapter: the adapter of that number, else
// the first one that isn't software), whatever API is then used for it.
//  - AMD integrated, measured on a Ryzen 7 9800X3D's Radeon: Vulkan 6 to 10% faster at the
//    game's own resolution, GPU time per frame 6 to 9% less (launcher/dev/gpu-weak-2026-10-27.md).
//    Integrated is Direct3D 12's own answer, D3D12_FEATURE_DATA_ARCHITECTURE.UMA, which
//    holds for an APU however much memory its BIOS sets aside for it (a size would not).
//  - AMD discrete: upstream issue #7 (RX 6600 XT, i5-10400) has Direct3D 12 20 to 25% faster,
//    Vulkan's GPU command thread costing more CPU. Not tried here.
//  - Intel: the SDK takes a slower path of rasterizer-ordered views for render targets on
//    Direct3D 12 and Vulkan uses ordinary framebuffer ones. Not measured: no Intel GPU.
//  - NVIDIA: Vulkan is no faster in races (the RTX 5080).
// The launcher's Graphics API setting repeats the rule (launcher/src/GpuApi.cs) so that it
// can mark the API recommended for the player's GPU; this and that must agree. Vulkan
// needs vulkan-1.dll and, for Automatic, a Vulkan device that is the same GPU (matched by
// LUID, else by vendor and device id): the SDK takes Vulkan's first device otherwise.
// Choosing the API (launcher, pause menu, --mcla_gpu_backend) skips the decision; only the
// recommendation, for the pause menu, is still worked out.
namespace {

struct AutomaticBackend {
  bool decided = false;
  bool vulkan = false;        // what Automatic does
  int vulkan_device = -1;     // that GPU's index among Vulkan's devices (-1: unknown)
  const char* recommended = nullptr;  // "vulkan" or "d3d12" for the GPU, nullptr if unknown
};
AutomaticBackend g_automatic;

constexpr UINT kVendorAmd = 0x1002;
constexpr UINT kVendorIntel = 0x8086;

// Direct3D 12's answer for an adapter: does the GPU share the CPU's memory (an APU)?
bool AdapterIsUma(IDXGIAdapter1* adapter) {
  HMODULE module = LoadLibraryW(L"d3d12.dll");  // (stays loaded: the SDK loads it again)
  if (!module) {
    return false;
  }
  const auto create_device =
      reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(module, "D3D12CreateDevice"));
  if (!create_device) {
    return false;
  }
  ID3D12Device* device = nullptr;
  if (FAILED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || !device) {
    return false;
  }
  D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
  const bool uma =
      SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                            sizeof(architecture))) &&
      architecture.UMA;
  device->Release();
  return uma;
}

// The index among Vulkan's physical devices of the GPU with this LUID (else vendor and
// device id); -1 if none. -2: Vulkan can't be used (no vulkan-1.dll or no instance).
int FindVulkanDevice(UINT vendor, UINT device_id, const LUID& luid) {
  HMODULE module = LoadLibraryW(L"vulkan-1.dll");  // (stays loaded: the SDK loads it again)
  if (!module) {
    return -2;
  }
  const auto get_instance_proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
  if (!get_instance_proc) {
    return -2;
  }
  const auto create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(get_instance_proc(nullptr, "vkCreateInstance"));
  if (!create_instance) {
    return -2;
  }
  VkApplicationInfo application{};
  application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  application.pApplicationName = "mcla";
  application.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo instance_info{};
  instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instance_info.pApplicationInfo = &application;
  VkInstance instance = VK_NULL_HANDLE;
  if (create_instance(&instance_info, nullptr, &instance) != VK_SUCCESS || !instance) {
    return -2;
  }
  const auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
      get_instance_proc(instance, "vkDestroyInstance"));
  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      get_instance_proc(instance, "vkEnumeratePhysicalDevices"));
  const auto get_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      get_instance_proc(instance, "vkGetPhysicalDeviceProperties"));
  const auto get_properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      get_instance_proc(instance, "vkGetPhysicalDeviceProperties2"));
  int found = -1;
  int by_ids = -1;
  uint32_t count = 0;
  if (enumerate && get_properties && enumerate(instance, &count, nullptr) == VK_SUCCESS &&
      count) {
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate(instance, &count, devices.data()) >= VK_SUCCESS) {
      for (uint32_t i = 0; i < count && found < 0; ++i) {
        VkPhysicalDeviceProperties properties{};
        get_properties(devices[i], &properties);
        if (get_properties2) {
          VkPhysicalDeviceIDProperties ids{};
          ids.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
          VkPhysicalDeviceProperties2 properties2{};
          properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
          properties2.pNext = &ids;
          get_properties2(devices[i], &properties2);
          if (ids.deviceLUIDValid && std::memcmp(ids.deviceLUID, &luid, sizeof(luid)) == 0) {
            found = int(i);
          }
        }
        if (by_ids < 0 && properties.vendorID == vendor && properties.deviceID == device_id) {
          by_ids = int(i);
        }
      }
    }
  }
  if (destroy_instance) {
    destroy_instance(instance, nullptr);
  }
  return found >= 0 ? found : by_ids;
}

}  // namespace

// Works out the rule above once: what Automatic does, which Vulkan device that is, and
// the API to recommend for the GPU. `automatic` is whether the player left the API on
// Automatic (only then is Vulkan's device looked for and the decision logged).
static void DecideAutomaticBackend(bool automatic) {
  if (g_automatic.decided) {
    return;
  }
  g_automatic.decided = true;
  const int adapter_option = std::atoi(rex::cvar::GetFlagByName("d3d12_adapter").c_str());
  if (adapter_option == -2) {  // WARP
    g_automatic.recommended = "d3d12";
    return;
  }
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
    return;
  }
  // The adapter Direct3D 12 would take. Only an AMD one is asked for its architecture (an
  // Intel one is Vulkan's whatever it is, anything else Direct3D 12's).
  DXGI_ADAPTER_DESC1 chosen{};
  bool have_chosen = false;
  bool uma = false;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    const bool described = SUCCEEDED(adapter->GetDesc1(&desc));
    const bool candidate =
        described && (adapter_option >= 0 ? int(i) == adapter_option
                                          : !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
    if (candidate && desc.VendorId == kVendorAmd) {
      uma = AdapterIsUma(adapter);
    }
    adapter->Release();
    adapter = nullptr;
    if (candidate) {
      chosen = desc;
      have_chosen = true;
      break;
    }
  }
  factory->Release();
  if (!have_chosen) {
    return;
  }

  const bool vulkan_gpu =
      chosen.VendorId == kVendorIntel || (chosen.VendorId == kVendorAmd && uma);
  char name[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, chosen.Description, -1, name, int(sizeof(name)) - 1, nullptr,
                      nullptr);
  const char* kind = chosen.VendorId == kVendorAmd
                         ? (uma ? "AMD integrated graphics" : "AMD discrete graphics")
                     : chosen.VendorId == kVendorIntel ? "Intel graphics"
                     : chosen.VendorId == 0x10DE       ? "NVIDIA graphics"
                                                       : "graphics";
  if (!vulkan_gpu) {
    g_automatic.recommended = "d3d12";
    if (automatic) {
      REXLOG_INFO("graphics API: Direct3D 12 (Automatic: {} '{}')", kind, name);
    }
    return;
  }
  // Vulkan, if it can be had: a driver, and a Vulkan device that is this GPU.
  const int device = automatic ? FindVulkanDevice(chosen.VendorId, chosen.DeviceId,
                                                  chosen.AdapterLuid)
                               : (LoadLibraryW(L"vulkan-1.dll") ? 0 : -2);
  if (device < 0) {
    g_automatic.recommended = "d3d12";
    if (automatic) {
      REXLOG_INFO(
          "graphics API: Direct3D 12 (Automatic: {} '{}', but {})", kind, name,
          device == -2 ? "Vulkan is not available" : "Vulkan lists no device for it");
    }
    return;
  }
  g_automatic.recommended = "vulkan";
  if (automatic) {
    g_automatic.vulkan = true;
    g_automatic.vulkan_device = device;
    REXLOG_INFO("graphics API: Vulkan (Automatic: {} '{}', Vulkan device {})", kind, name,
                device);
  }
}

// The API recommended for the player's GPU, "vulkan" or "d3d12" (nullptr if the GPU isn't
// known); the pause menu marks it.
const char* MclaRecommendedBackend() {
  return g_automatic.recommended;
}
#endif

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
  DecideAutomaticBackend(backend == "any");
  if (backend == "any" && g_automatic.vulkan) {
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
#ifdef _WIN32
  // Automatic took Vulkan for the GPU Direct3D 12 would use: Vulkan's own choice (its first
  // device) must be that GPU too. A vulkan_device given by hand wins.
  if (g_automatic.vulkan && g_automatic.vulkan_device >= 0 &&
      rex::cvar::GetFlagSource("vulkan_device") == rex::cvar::Source::kDefault &&
      !rex::cvar::SetDefaultValue("vulkan_device", std::to_string(g_automatic.vulkan_device))) {
    REXLOG_WARN("graphics API: vulkan_device = {} not taken", g_automatic.vulkan_device);
  }
#endif

  // Development: scripted controller input (src/dev_pad.cpp), off by default.
  MclaDevPadConfigure(config);
}

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

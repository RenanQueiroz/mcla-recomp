#include "generated/default/mcla_init.h"

#include "mcla_app.h"
#include "automatic_api.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
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
#include <dxgi1_6.h>
#include <shellapi.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "gpu_choice.h"
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
#ifdef _WIN32
REXCVAR_DEFINE_BOOL(mcla_prefer_dedicated_gpu, true, "MCLA",
                    "On a PC with more than one GPU (a laptop with integrated and dedicated "
                    "graphics): run on the GPU Windows' graphics settings give this app, else on "
                    "the high-performance one, instead of the first adapter Windows lists (often "
                    "the integrated one). false takes that first adapter, as before. A "
                    "d3d12_adapter or vulkan_device you set always wins. Takes effect at the next "
                    "start")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// Read by NVIDIA's Optimus and AMD's PowerXpress drivers when the exe loads: on a laptop with
// switchable graphics they run the program on the dedicated GPU (src/gpu_choice.h has the
// game's own choice). Not tied to mcla_prefer_dedicated_gpu: an exported variable is fixed.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif


std::unique_ptr<rex::system::IAudioSystem> CreateMclaAudioSystem(
    rex::runtime::FunctionDispatcher* function_dispatcher);
void MclaDevPadConfigure(rex::RuntimeConfig& config);
void MclaDevPadStart(rex::ui::Window* window, rex::ui::ImGuiDrawer* imgui_drawer);
void MclaSaveMenuSettings();  // src/settings_menu.cpp
bool MclaPrepareShadersIsRequested();  // src/shader_prepare.cpp
void MclaShaderStorageAfterPreload(rex::Runtime* runtime);
void MclaMigrateAntiAliasing();  // src/perf_options.cpp
void MclaApplyPreset();  // src/perf_options.cpp
void MclaApplyGraphicsFixDefaults(bool vulkan);  // src/perf_options.cpp

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
// The game's shutdown has begun: the audio silence from here on is the output stopping (src/poll_wait.cpp).
void MclaPumpWakeShutdownBegins();

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
// Vulkan on Intel's GPUs and on AMD's integrated GPUs of the RDNA generation (the ones that
// share the CPU's memory and whose smallest wave is 32 lanes); Direct3D 12 on AMD's discrete
// cards, on AMD's integrated GCN (Vega) GPUs, on NVIDIA and when nothing is known. The rule
// itself is src/automatic_api.h (apart from Windows, so a test can run it). Which GPU: the
// one the game uses (ChooseGpu, src/gpu_choice.h: Windows' graphics settings for this exe, else
// the high-performance GPU; the SDK's d3d12_adapter, when set, wins), whatever API is then used
// for it. A software adapter
// (Windows' WARP: d3d12_adapter = -2, or its number) is Direct3D 12's, its vendor being
// Microsoft's, neither AMD's nor Intel's: Vulkan has no device for it, so the recommendation
// is Direct3D 12; a number that names no adapter gets none (the launcher, ShaderStamp.cs,
// answers the same for all of these).
//  - AMD integrated, RDNA, measured on a Ryzen 7 9800X3D's Radeon (RDNA 2): Vulkan 6 to 10%
//    faster at the game's own resolution, GPU time per frame 6 to 9% less
//    (launcher/dev/gpu-weak-2026-10-27.md). Integrated is Direct3D 12's own answer,
//    D3D12_FEATURE_DATA_ARCHITECTURE.UMA, which holds for an APU however much memory its BIOS
//    sets aside for it (a size would not); RDNA is told by D3D12_FEATURE_DATA_D3D12_OPTIONS1's
//    WaveLaneCountMin, 32 (GCN reports 64).
//  - AMD integrated, GCN (Vega: the Ryzen 3000G to 5000G APUs): Direct3D 12. A user's
//    Ryzen 7 5700G (Vega 8) had the HUD, the menus and parts of the city missing on Vulkan
//    (fork issue #14). Not tried here: no Vega GPU was available.
//  - AMD discrete: upstream issue #7 (RX 6600 XT, i5-10400) has Direct3D 12 20 to 25% faster,
//    Vulkan's GPU command thread costing more CPU. Not tried here.
//  - Intel: the SDK takes a slower path of rasterizer-ordered views for render targets on
//    Direct3D 12 and Vulkan uses ordinary framebuffer ones. Not measured: no Intel GPU.
//  - NVIDIA: Vulkan is no faster in races (the RTX 5080).
// The launcher's Graphics API setting repeats the rule and the choice of the GPU
// (launcher/src/ShaderStamp.cs) so that it can mark the API recommended for the player's GPU;
// this and that must agree. Vulkan
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

// Direct3D 12's answers for an adapter (src/automatic_api.h): does the GPU share the CPU's
// memory (an APU), and what is the smallest wave it runs (32 on RDNA, 64 on GCN)? A device
// that can't be made, or a query that fails, leaves the answer 0 / false: Direct3D 12.
void ReadAdapterFacts(IDXGIAdapter1* adapter, mcla::automatic_api::AdapterFacts& facts) {
  HMODULE module = LoadLibraryW(L"d3d12.dll");  // (stays loaded: the SDK loads it again)
  if (!module) {
    return;
  }
  const auto create_device =
      reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(module, "D3D12CreateDevice"));
  if (!create_device) {
    return;
  }
  ID3D12Device* device = nullptr;
  if (FAILED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || !device) {
    return;
  }
  D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
  facts.uma = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture,
                                                    sizeof(architecture))) &&
              architecture.UMA;
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options1,
                                            sizeof(options1)))) {
    facts.wave_lane_count_min = options1.WaveOps ? options1.WaveLaneCountMin : 0;
  }
  device->Release();
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

// --- Which GPU (src/gpu_choice.h) ---------------------------------------------------------

std::string ToUtf8(const wchar_t* text) {
  char name[512] = {};
  WideCharToMultiByte(CP_UTF8, 0, text, -1, name, int(sizeof(name)) - 1, nullptr, nullptr);
  return name;
}

// Whether Direct3D 12 can make a device on the adapter at feature level 11_0, which the SDK
// requires of the adapter it takes (src/ui/d3d12/d3d12_provider.cpp).
bool CanMakeD3d12Device(IDXGIAdapter1* adapter) {
  HMODULE module = LoadLibraryW(L"d3d12.dll");  // (stays loaded: the SDK loads it again)
  if (!module) {
    return false;
  }
  const auto create_device =
      reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(module, "D3D12CreateDevice"));
  return create_device &&
         SUCCEEDED(create_device(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr));
}

// The exe's entry in Windows' graphics settings (Settings > System > Display > Graphics): a value
// named by the exe's full path, with data such as "GpuPreference=2;". False if there is none.
bool ReadWindowsGraphicsEntry(std::string* exe, std::string* data) {
  wchar_t path[MAX_PATH * 4] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  if (length == 0 || length >= std::size(path)) {
    return false;
  }
  *exe = ToUtf8(path);
  wchar_t value[512] = {};
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", path,
                   RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
    return false;
  }
  *data = ToUtf8(value);
  return true;
}

struct ChosenGpu {
  bool done = false;
  bool have = false;  // an adapter was chosen
  bool prefer_dedicated = true;
  int index = -1;  // its number in IDXGIFactory1::EnumAdapters1's order (the SDK's d3d12_adapter)
  DXGI_ADAPTER_DESC1 desc{};
  std::string name;
  mcla::gpu_choice::Choice choice;
  mcla::automatic_api::AdapterFacts facts;  // vendor, and for AMD's what Direct3D 12 says
};
ChosenGpu g_gpu;

bool SameLuid(const LUID& a, const LUID& b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

// Lists the adapters, applies the rule of src/gpu_choice.h and logs both: the lines to ask for
// when someone says the game is slow ("GPU: ..." in the log). Run once, before the API is
// decided. Sets nothing: ApplyGpuChoice does, once the GPU plugin is loaded.
void ChooseGpu() {
  using namespace mcla::gpu_choice;
  if (g_gpu.done) {
    return;
  }
  g_gpu.done = true;
  g_gpu.prefer_dedicated = REXCVAR_GET(mcla_prefer_dedicated_gpu);
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
    REXLOG_WARN("GPU: DXGI could not be started; the SDK takes the adapter");
    return;
  }

  // The adapters in the order the SDK numbers them (d3d12_adapter).
  std::vector<DXGI_ADAPTER_DESC1> descs;
  std::vector<Adapter> adapters;
  std::vector<char> described;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    const bool ok = SUCCEEDED(adapter->GetDesc1(&desc));
    adapter->Release();
    adapter = nullptr;
    if (!ok) {
      desc = {};
    }
    descs.push_back(desc);
    described.push_back(ok);
    adapters.push_back({(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0});
  }
  REXLOG_INFO("GPU: Windows lists {} adapter(s), in the order of d3d12_adapter:", descs.size());
  for (size_t i = 0; i < descs.size(); ++i) {
    REXLOG_INFO("GPU:   [{}] '{}' vendor 0x{:04X} device 0x{:04X}, {} MiB dedicated video memory{}",
                i, ToUtf8(descs[i].Description), descs[i].VendorId, descs[i].DeviceId,
                uint64_t(descs[i].DedicatedVideoMemory) >> 20,
                adapters[i].software ? ", software" : "");
  }

  // Windows' ranking of them, by IDXGIFactory6 (Windows 10 1803 and later), as indices.
  std::vector<int> high_performance, power_saving;
  IDXGIFactory6* factory6 = nullptr;
  if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6))) && factory6) {
    const auto rank = [&](DXGI_GPU_PREFERENCE preference, std::vector<int>& out,
                          const char* what) {
      IDXGIAdapter1* listed = nullptr;
      std::string line;
      for (UINT i = 0; factory6->EnumAdapterByGpuPreference(i, preference,
                                                           IID_PPV_ARGS(&listed)) == S_OK;
           ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(listed->GetDesc1(&desc))) {
          for (size_t k = 0; k < descs.size(); ++k) {
            if (SameLuid(desc.AdapterLuid, descs[k].AdapterLuid)) {
              out.push_back(int(k));
              line += fmt::format("{}{}", line.empty() ? "" : ", ", k);
              break;
            }
          }
        }
        listed->Release();
        listed = nullptr;
      }
      REXLOG_INFO("GPU: Windows ranks them for {}: {}", what, line.empty() ? "none" : line);
    };
    rank(DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, high_performance, "high performance");
    rank(DXGI_GPU_PREFERENCE_MINIMUM_POWER, power_saving, "power saving");
    factory6->Release();
  } else {
    REXLOG_INFO("GPU: this Windows has no IDXGIFactory6 (no ranking of the adapters)");
  }

  Request request;
  request.prefer_dedicated = g_gpu.prefer_dedicated;
  const std::string option_text = rex::cvar::GetFlagByName("d3d12_adapter");
  request.adapter_option = std::atoi(option_text.c_str());
  request.adapter_option_set = !option_text.empty() && rex::cvar::GetFlagSource("d3d12_adapter") !=
                                                           rex::cvar::Source::kDefault;
  std::string exe, entry;
  if (ReadWindowsGraphicsEntry(&exe, &entry)) {
    request.windows_preference = ParseWindowsPreference(entry);
    REXLOG_INFO("GPU: Windows' graphics settings for '{}': '{}'", exe, entry);
  } else {
    REXLOG_INFO("GPU: Windows' graphics settings have no entry for '{}'", exe);
  }

  std::vector<signed char> usable_cache(adapters.size(), -1);
  const auto usable = [&](int i) {
    if (i < 0 || size_t(i) >= adapters.size() || !described[i]) {
      return false;
    }
    if (usable_cache[i] < 0) {
      IDXGIAdapter1* candidate = nullptr;
      usable_cache[i] = factory->EnumAdapters1(UINT(i), &candidate) == S_OK && candidate &&
                        CanMakeD3d12Device(candidate);
      if (candidate) {
        candidate->Release();
      }
    }
    return usable_cache[i] != 0;
  };
  g_gpu.choice = Choose(adapters, high_performance, power_saving, request, usable);
  const Choice& choice = g_gpu.choice;
  if (choice.index < 0) {
    if (choice.why == Why::kUserIndex || choice.why == Why::kUserSoftware) {
      REXLOG_WARN("GPU: d3d12_adapter = {} set by the user names no adapter that runs "
                  "Direct3D 12 at feature level 11_0", request.adapter_option);
    } else {
      REXLOG_WARN("GPU: no adapter runs Direct3D 12 at feature level 11_0");
    }
    factory->Release();
    return;
  }
  g_gpu.have = true;
  g_gpu.index = choice.index;
  g_gpu.desc = descs[choice.index];
  g_gpu.name = ToUtf8(g_gpu.desc.Description);
  g_gpu.facts.vendor = g_gpu.desc.VendorId;
  if (g_gpu.desc.VendorId == mcla::automatic_api::kVendorAmd) {
    IDXGIAdapter1* chosen = nullptr;
    if (factory->EnumAdapters1(UINT(choice.index), &chosen) == S_OK && chosen) {
      ReadAdapterFacts(chosen, g_gpu.facts);
      chosen->Release();
    }
  }
  factory->Release();

  std::string why;
  switch (choice.why) {
    case Why::kUserIndex:
      why = fmt::format("d3d12_adapter = {} set by the user", request.adapter_option);
      break;
    case Why::kUserSoftware:
      why = "d3d12_adapter = -2 set by the user: the software adapter";
      break;
    case Why::kUserFirst:
      why = fmt::format("d3d12_adapter = {} set by the user: the first adapter Windows lists",
                        request.adapter_option);
      break;
    case Why::kOff:
      why = "mcla_prefer_dedicated_gpu is off: the first adapter Windows lists";
      break;
    case Why::kWindowsPowerSaving:
      why = "Windows' graphics settings ask for power saving for this app";
      break;
    case Why::kWindowsHighPerformance:
      why = "Windows' graphics settings ask for high performance for this app";
      break;
    case Why::kHighPerformance:
      why = "the high-performance GPU";
      break;
    case Why::kFirstListed:
      why = high_performance.empty() ? "the first adapter Windows lists, as it ranks none"
                                     : "the first adapter Windows lists";
      break;
    case Why::kNone:
      break;
  }
  if (choice.set_default && choice.first_index >= 0 && choice.first_index != choice.index) {
    why += fmt::format("; Windows lists '{}' first", ToUtf8(descs[choice.first_index].Description));
  }
  REXLOG_INFO("GPU: '{}' ({})", g_gpu.name, why);
}

}  // namespace

// Works out the rule above once: what Automatic does, which Vulkan device that is, and
// the API to recommend for the GPU. `automatic` is whether the player left the API on
// Automatic (only then is Vulkan's device looked for and the decision logged). The GPU is
// ChooseGpu's.
static void DecideAutomaticBackend(bool automatic) {
  if (g_automatic.decided) {
    return;
  }
  g_automatic.decided = true;
  ChooseGpu();
  if (!g_gpu.have) {
    return;
  }
  const DXGI_ADAPTER_DESC1& chosen = g_gpu.desc;
  const char* name = g_gpu.name.c_str();
  const bool vulkan_gpu = mcla::automatic_api::PrefersVulkan(g_gpu.facts);
  const char* kind = mcla::automatic_api::KindOf(g_gpu.facts);
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

// Once the GPU plugin is loaded: the chosen GPU becomes d3d12_adapter's default (a number the
// player set is left alone, and nothing is written to mcla.toml), and when the API is Vulkan,
// Automatic's or the player's, vulkan_device's default becomes the Vulkan device of that same
// GPU (matched by LUID, else by vendor and device id) instead of the loader's first.
// mcla_prefer_dedicated_gpu off leaves both as they were before: only Automatic's Vulkan sets
// vulkan_device.
static void ApplyGpuChoice(bool vulkan) {
  if (!g_gpu.have) {
    return;
  }
  if (g_gpu.choice.set_default &&
      !rex::cvar::SetDefaultValue("d3d12_adapter", std::to_string(g_gpu.index))) {
    REXLOG_WARN("GPU: d3d12_adapter = {} not taken", g_gpu.index);
  }
  if (!vulkan) {
    return;
  }
  if (rex::cvar::GetFlagSource("vulkan_device") != rex::cvar::Source::kDefault) {
    REXLOG_INFO("GPU: vulkan_device = {} set by the user",
                rex::cvar::GetFlagByName("vulkan_device"));
    return;
  }
  int device = g_automatic.vulkan ? g_automatic.vulkan_device : -1;
  if (!g_automatic.vulkan && g_gpu.prefer_dedicated) {
    device = FindVulkanDevice(g_gpu.desc.VendorId, g_gpu.desc.DeviceId, g_gpu.desc.AdapterLuid);
  }
  if (device >= 0) {
    if (rex::cvar::SetDefaultValue("vulkan_device", std::to_string(device))) {
      REXLOG_INFO("GPU: Vulkan device {} is '{}'", device, g_gpu.name);
    } else {
      REXLOG_WARN("GPU: vulkan_device = {} not taken", device);
    }
  } else if (g_gpu.prefer_dedicated) {
    REXLOG_INFO("GPU: {} for '{}'; vulkan_device stays at the loader's first",
                device == -2 ? "Vulkan is not available" : "Vulkan lists no device", g_gpu.name);
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
  ChooseGpu();
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
#ifdef _WIN32
  const bool vulkan_backend = backend == "vulkan" && config.graphics;
#endif
  if (!config.graphics) {
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin);
  }
  ApplyPcDefaults();
  MclaMigrateAntiAliasing();
  MclaApplyPreset();
#ifdef _WIN32
  // The GPU the game chose (ChooseGpu) is the one the SDK takes, whatever the API: d3d12_adapter
  // for Direct3D 12, vulkan_device for Vulkan (Automatic's or the player's), whose own choice
  // is its first device. A d3d12_adapter or vulkan_device given by hand wins.
  ApplyGpuChoice(vulkan_backend);
#endif

  // Development: scripted controller input (src/dev_pad.cpp), off by default.
  MclaDevPadConfigure(config);
}

#ifdef _WIN32
// How many logical processors the process may run on: the bits of its affinity mask
// (GetProcessAffinityMask, which includes the limit of a job object, as a launcher or a test
// harness puts on a process). A mask that is the whole processor group says nothing about the
// other groups of a machine with more than 64 processors, so then every group is counted.
// 0 if it cannot be told.
static uint32_t LogicalProcessorsAvailable() {
  DWORD_PTR process = 0;
  DWORD_PTR system = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system) || process == 0) {
    return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  }
  if (process == system) {
    const DWORD all = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (all != 0) {
      return all;
    }
  }
  uint32_t count = 0;
  for (; process != 0; process &= process - 1) {
    ++count;
  }
  return count;
}

// The two CPU options whose best setting depends on how many logical processors there are
// (README.md, "Turning a change off"; launcher/dev/profiling-2026-10-26.md, "Wake on demand"
// and "The timer queue and the frame-pacing thread by processor count"; A/B of 3 runs a side
// at 16, 8, 6 and 4 logical processors, uncapped and at caps of 120, 60 and 30):
// - 6 or more: mcla_pump_wake_on_demand. It lifts the 145 FPS the game's own Sleep holds the
//   uncapped game to (+49 to +65%, the caps unchanged, no audio underrun at 16, 8 and 6); at 4
//   logical it gained only 11 to 16%, and one run in three ran the audio dry for 2 frames and
//   cap 120 had a 19% longer p99, so it is left off there.
// - 5 or fewer: timer_queue_hr_wait. It gives back the core the timer thread spins on: at 4
//   logical +17 to 21% FPS uncapped, p99 27-30 ms to 10-12 ms, cap 120 held (117 to 120), the
//   process 23 to 58% less CPU and the late audio wake-ups ~300 to ~20 a run, with no
//   underrun. At 6 it saved 31 to 50% of the CPU and left the frame 1.7% longer when driving,
//   and at 16 and 8 it made the frame 1 to 2.4% longer, so it is left off from 6 up.
// Each follows the usual rule: a value from the command line, the environment or mcla.toml wins
// (SetDefaultValue then only records the default), and none is saved. Both are read where they
// are used (the timer thread at every wait, the pump thread at every round), so the default
// set here is in effect for the whole run. Not on Linux and the PS5, which keep both off.
static void ApplyCpuDefaults() {
  constexpr uint32_t kWakeOnDemandFrom = 6;
  const uint32_t logical = LogicalProcessorsAvailable();
  if (logical == 0) {
    REXLOG_WARN("CPU: could not count the logical processors; both options keep their defaults (off)");
    return;
  }
  const bool many = logical >= kWakeOnDemandFrom;
  struct Option {
    const char* name;
    const char* label;
    bool automatic_on;
  };
  const Option options[] = {
      {"timer_queue_hr_wait", "timer queue high-resolution wait", !many},
      {"mcla_pump_wake_on_demand", "pump wake on demand", many},
  };
  std::string text;
  bool any_automatic = false;
  for (const Option& option : options) {
    const bool by_player = rex::cvar::GetFlagSource(option.name) != rex::cvar::Source::kDefault;
    if (option.automatic_on && !rex::cvar::SetDefaultValue(option.name, "true")) {
      REXLOG_WARN("CPU: {} = true not taken", option.name);
    }
    if (!text.empty()) {
      text += ", ";
    }
    text += fmt::format("{} {}{}", option.label,
                        rex::cvar::GetFlagByName(option.name) == "true" ? "on" : "off",
                        by_player ? " (set by the player)" : "");
    any_automatic = any_automatic || !by_player;
  }
  REXLOG_INFO("CPU: {} logical processors: {}{}", logical, text,
              any_automatic ? (many ? " (automatic; 6 or more)" : " (automatic; 5 or fewer)") : "");
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
  // - video memory above native resolution (fork issue #19, launcher/dev/
  //   vram-2026-10-28.md): no host render target taller than 1,024 rows (the
  //   game's tallest surface is 736; the SDK makes each as tall as the
  //   console's whole EDRAM, 4 to 16 times that for small ones), those that
  //   hold no data and have been idle for 60 seconds freed, and, when the
  //   process uses 90% of its video memory budget, those idle for 2 seconds freed and
  //   the texture cache's limits halved;
  // - on Windows, D3D12 uploads into the shared memory recorded at the start of
  //   the run of draws that needs them (d3d12_hoist_shared_memory_uploads,
  //   patches/README.md, launcher/dev/d3d12-upload-hoisting-2026-10-28.md):
  //   no pixel difference beyond the world's own flicker in the checks made,
  //   the GPU frame shorter. The SDK's own default stays off; Vulkan has its own;
  // - anisotropic filtering at 16x instead of the SDK's 4x: the only level
  //   never below what the game asks for (it asks 16x for the road in a race),
  //   at no measurable cost on an RTX 5080 and 1.5% on an integrated Radeon
  //   (launcher/dev/aniso-2026-10-27.md);
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
      {"render_target_max_height", "1024"},
      {"render_target_trim_seconds", "60"},
      {"gpu_memory_pressure_percent", "90"},
      {"anisotropic_override", "5"},
#if REX_PLATFORM_WIN32
      {"d3d12_hoist_shared_memory_uploads", "true"},
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
#ifdef _WIN32
  ApplyCpuDefaults();
#endif
  // Fixes that are defaults of SDK options too: the custom vinyls' readback, and on Vulkan the depth conversion
  // (src/perf_options.cpp). "any" is what Automatic decided on Windows (DecideAutomaticBackend, already run by
  // OnPreSetup); elsewhere the plugin has only Vulkan.
  const std::string backend = REXCVAR_GET(mcla_gpu_backend);
#ifdef _WIN32
  const bool any_is_vulkan = g_automatic.vulkan;
#else
  const bool any_is_vulkan = true;
#endif
  MclaApplyGraphicsFixDefaults(backend == "vulkan" || (backend == "any" && any_is_vulkan));
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
  MclaPumpWakeShutdownBegins();  // the window closes now: the title is terminated next
  return true;
}

void MclaApp::OnGuestThreadExit(rex::system::XThread* thread) {
  (void)thread;
  MclaPumpWakeShutdownBegins();
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

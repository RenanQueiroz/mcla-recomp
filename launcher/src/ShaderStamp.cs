// Whether shaders should be prepared before Play. After every start and every preparation the
// game records what its storage was loaded with (the stamp, <cache>\shaders\local\545407F8.stamp.json,
// src/shader_prepare.cpp): the shader pack, the API and resolution scale it was asked for (and
// the ones it ended up with), the adapter and its driver, the files' sizes. This compares it with
// what the next start will use: the pack, mcla.toml and the options the launcher passes, the
// display drivers installed now and the files as they are. It reads a few small files and lists
// the display adapters (DXGI); it never starts mcla.exe. Kept free of the launcher's UI, for the
// setup wizard too.
//
// What it doesn't notice, because the launcher can't tell from here (the player runs More >
// Prepare shaders): a graphics API left on "any" that the game resolves differently than the
// launcher expects (the plugin failing to load, no Vulkan device for the GPU); a switch between
// two adapters that both stay installed and use the same API (the game's choice of the GPU,
// DisplayDrivers.ChooseGpu, is repeated here, but the stamp only says which adapter was used, not
// whether it is the one to use now); a storage format change without a new pack (the game then
// refuses the pack, which no preparation fixes).
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using Microsoft.Win32;

namespace MclaLauncher;

/// Why the shaders should be prepared before the next start (null: they needn't be), and how long
/// finding out took.
sealed record PrepareCheck(string? Reason, long Milliseconds);

static class ShaderStamp
{
    const string Title = "545407F8";
    static readonly string[] StorageFiles = ["xsh", "rtv.d3d12.xpso", "rov.d3d12.xpso", "fbo.vk.xpso", "fsi.vk.xpso"];

    /// The pack's fingerprint as the game records it: the XXH3 its last 8 bytes hold.
    public static string? PackHash(string packFile)
    {
        try
        {
            using var file = new FileStream(packFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            if (file.Length < 16) return null;
            file.Seek(-8, SeekOrigin.End);
            Span<byte> bytes = stackalloc byte[8];
            file.ReadExactly(bytes);
            return BitConverter.ToUInt64(bytes).ToString("X16", CultureInfo.InvariantCulture);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// A setting as the game will take it: the options the launcher passes, then a REX_ environment
    /// variable, then mcla.toml (the SDK's order); null when none sets it.
    public static string? Setting(string name, string arguments, IReadOnlyDictionary<string, string> toml)
    {
        if (GameOptions.Value(arguments, name) is string option) return option;
        if (Environment.GetEnvironmentVariable("REX_" + name.ToUpperInvariant()) is { Length: > 0 } variable) return variable;
        if (toml.TryGetValue(name, out var raw)) return Convert.ToString(SettingValues.Parse(raw), CultureInfo.InvariantCulture);
        return null;
    }

    /// The options that decide which GPU the game uses (src/gpu_choice.h), as the game will take them:
    /// d3d12_adapter where something sets it (the options the launcher passes, a REX_ variable or
    /// mcla.toml; null if none does, or the value is not a number), mcla_prefer_dedicated_gpu (on unless
    /// something turns it off), and the exe whose entry in Windows' graphics settings counts: mcla.exe in
    /// the game's folder.
    public static DisplayDrivers.GpuOptions GpuOptionsFor(string arguments, IReadOnlyDictionary<string, string> toml, string workingDirectory)
    {
        int? adapter = int.TryParse(Setting("d3d12_adapter", arguments, toml), NumberStyles.Integer, CultureInfo.InvariantCulture, out var number) ? number : null;
        var prefer = !(bool.TryParse(Setting("mcla_prefer_dedicated_gpu", arguments, toml), out var value) && !value);
        string? exe = null;
        try { exe = Path.GetFullPath(Path.Combine(workingDirectory, "mcla.exe")); }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException) { }
        return new(adapter, prefer, exe);
    }

    /// A folder option the way the game takes it. The SDK reads cache_root and user_data_root
    /// before mcla.toml is loaded (ReXApp::SetupEnvironment), so only the command line and the
    /// REX_ variables set them; the toml does not. A relative path is the game's working folder's,
    /// which the launcher sets to mcla.exe's folder.
    static string? PathSetting(string name, string arguments, string workingDirectory)
    {
        var value = GameOptions.Value(arguments, name);
        if (string.IsNullOrEmpty(value)) value = Environment.GetEnvironmentVariable("REX_" + name.ToUpperInvariant());
        return string.IsNullOrEmpty(value) ? null : Path.GetFullPath(value, workingDirectory);
    }

    /// The cache folder the game will use, as the SDK picks it: cache_root, else user_data_root's
    /// cache folder, else Documents\mcla\cache. Throws ArgumentException or NotSupportedException
    /// for a path that is malformed.
    public static string CacheRoot(string arguments, string workingDirectory)
    {
        if (PathSetting("cache_root", arguments, workingDirectory) is { } cache) return cache;
        var user = PathSetting("user_data_root", arguments, workingDirectory)
            ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "mcla");
        return Path.Combine(user, "cache");
    }

    /// The shader pack the game will use: mcla_shader_pack where something sets it, else the one
    /// the fork ships. Throws as CacheRoot.
    public static string PackFile(string repo, string arguments, IReadOnlyDictionary<string, string> toml, string workingDirectory) =>
        Setting("mcla_shader_pack", arguments, toml) is { Length: > 0 } custom
            ? Path.GetFullPath(custom, workingDirectory)
            : ShaderPrepRun.PackFile(repo);

    public static PrepareCheck Check(string repo, string workingDirectory, string arguments, IReadOnlyDictionary<string, string> toml, string? skipPack)
    {
        var watch = Stopwatch.StartNew();
        string? reason;
        try
        {
            reason = Reason(repo, workingDirectory, arguments, toml, skipPack);
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException)
        {
            // A path option the game can't use either (a bad character): a preparation couldn't
            // do anything about it, so Play goes on.
            Debug.WriteLine($"shader check: {ex.Message}");
            reason = null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException
                                       or InvalidOperationException or FormatException or KeyNotFoundException)
        {
            // A stamp that can't be read is as good as none.
            reason = "the shader storage's record can't be read";
        }
        return new(reason, watch.ElapsedMilliseconds);
    }

    // The record's vendor, kept for as long as the file is as it was (the settings page asks at
    // every render, the sliders' drags among them): a stat per ask, a parse per change.
    static ((string File, DateTime Written, long Length) Key, uint Vendor)? lastAdapterVendor;

    /// The vendor of the adapter the game used at its last start, from its shader record (0: no
    /// record, or none with an adapter in it).
    public static uint LastAdapterVendor(string arguments, string workingDirectory)
    {
        try
        {
            var stampFile = Path.Combine(CacheRoot(arguments, workingDirectory), "shaders", "local", Title + ".stamp.json");
            var info = new FileInfo(stampFile);
            if (!info.Exists) return 0;
            var key = (stampFile, info.LastWriteTimeUtc, info.Length);
            if (lastAdapterVendor is { } known && known.Key == key) return known.Vendor;
            uint found = 0;
            using (var stamp = JsonDocument.Parse(ReadShared(stampFile)))
            {
                if (stamp.RootElement.TryGetProperty("storage", out var storage) && storage.ValueKind == JsonValueKind.Object &&
                    storage.TryGetProperty("adapter", out var adapter) && adapter.ValueKind == JsonValueKind.Object &&
                    adapter.TryGetProperty("vendor", out var vendor) && vendor.ValueKind == JsonValueKind.Number)
                    found = vendor.GetUInt32();
            }
            lastAdapterVendor = (key, found);
            return found;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException
                                       or NotSupportedException or InvalidOperationException or FormatException)
        {
            // No record is as good as one without an adapter.
        }
        return 0;
    }

    static string? JsonText(JsonElement parent, string name) =>
        parent.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    static string? Reason(string repo, string workingDirectory, string arguments, IReadOnlyDictionary<string, string> toml, string? skipPack)
    {
        var packFile = PackFile(repo, arguments, toml, workingDirectory);
        if (!File.Exists(packFile)) return null;
        var pack = PackHash(packFile);
        // The last automatic preparation failed for this pack: not again at every Play
        // (More > Prepare shaders tries again).
        if (pack != null && pack == skipPack) return null;

        var cache = CacheRoot(arguments, workingDirectory);
        var local = Path.Combine(cache, "shaders", "local");
        var shareable = Path.Combine(cache, "shaders", "shareable");

        // The game's last start ended while it was adding the pack's shaders.
        var journal = Path.Combine(local, Title + ".import");
        if (File.Exists(journal) && ReadShared(journal).Split('\n').Any(line => line.Trim() is "phase merged" or "phase rebuild"))
            return "the last start ended while shaders were being added";

        var stampFile = Path.Combine(local, Title + ".stamp.json");
        if (!File.Exists(stampFile)) return "shaders haven't been prepared yet";
        using var stamp = JsonDocument.Parse(ReadShared(stampFile));
        var root = stamp.RootElement;

        // The pack the game added at its last start. With the automatic import off (mcla_shader_pack_auto
        // false) a normal start adds nothing and records no pack: it only says something about the
        // pack when the last run was a preparation, which imports whatever that option says.
        var stampPack = JsonText(root, "pack");
        var auto = !bool.TryParse(Setting("mcla_shader_pack_auto", arguments, toml), out var autoValue) || autoValue;
        if (pack != null && stampPack != pack && (auto || JsonText(root, "pack_import") == "on"))
            return "the shader pack changed";

        // The files: one that vanished or shrank was deleted, reset or cut.
        if (root.TryGetProperty("files", out var files) && files.ValueKind == JsonValueKind.Object)
        {
            foreach (var file in files.EnumerateObject())
            {
                if (!StorageFiles.Contains(file.Name)) continue;
                var info = new FileInfo(Path.Combine(shareable, Title + "." + file.Name));
                var size = file.Value.GetProperty("size").GetInt64();
                if (!info.Exists) return "the shader cache was deleted";
                if (info.Length < size) return "the shader cache was reset";
            }
        }

        if (!root.TryGetProperty("storage", out var storage) || storage.ValueKind != JsonValueKind.Object) return null;

        // The API and the resolution scale the next start will ask for, against what the last one
        // was asked for ("settings"): not against what it ended up with ("storage"), which the game
        // changes by itself on a weaker GPU (a scale it can't do) or when a plugin fails to load,
        // and which would then never match.
        if (root.TryGetProperty("settings", out var asked) && asked.ValueKind == JsonValueKind.Object)
        {
            var api = (Setting("mcla_gpu_backend", arguments, toml) ?? "any").Trim().ToLowerInvariant();
            var askedApi = (JsonText(asked, "mcla_gpu_backend") ?? "any").Trim().ToLowerInvariant();
            // "any" then, an explicit API now: the same API as the one it came to, is no change.
            var sameAsResolved = askedApi == "any" && api == JsonText(storage, "api")?.ToLowerInvariant();
            if (api != askedApi && !sameAsResolved) return "the graphics API changed";
            // "any" then and now: what Automatic comes to depends on the GPU and on the game's version
            // (a cache made when it was Direct3D 12 for every GPU is not for the Vulkan it is now on
            // AMD's RDNA integrated GPUs and Intel's, nor one made when Vega's integrated GPUs were
            // Vulkan's for the Direct3D 12 they are now; nor one made when a laptop's integrated GPU
            // was the one the game ran on, and it is the dedicated one now).
            if (api == "any" && askedApi == "any" && JsonText(storage, "api")?.ToLowerInvariant() is { } resolved)
            {
                if (DisplayDrivers.AutomaticApi(GpuOptionsFor(arguments, toml, workingDirectory)) is { } automatic && automatic != resolved) return "the graphics API changed";
            }

            var scaleText = (Setting("resolution_scale", arguments, toml) ?? "1").Trim();
            var askedScaleText = (JsonText(asked, "resolution_scale") ?? "1").Trim();
            var scaleChanged = int.TryParse(scaleText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var scale) &&
                               int.TryParse(askedScaleText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var askedScale)
                ? scale != askedScale
                : scaleText != askedScaleText;
            if (scaleChanged) return "the resolution changed";
        }

        // The adapter and its driver: a driver update empties the driver's own cache.
        if (storage.TryGetProperty("adapter", out var adapter) && adapter.TryGetProperty("vendor", out var vendorElement) &&
            adapter.TryGetProperty("device", out var deviceElement))
        {
            uint vendor = vendorElement.GetUInt32(), device = deviceElement.GetUInt32();
            var now = DisplayDrivers.List();
            if (now.Count > 0)
            {
                var current = now.FirstOrDefault(a => a.Vendor == vendor && a.Device == device);
                if (current == null) return "the graphics card changed";
                if (root.TryGetProperty("display_drivers", out var drivers) && drivers.ValueKind == JsonValueKind.Array)
                {
                    var recorded = drivers.EnumerateArray().FirstOrDefault(d =>
                        d.GetProperty("vendor").GetUInt32() == vendor && d.GetProperty("device").GetUInt32() == device);
                    if (recorded.ValueKind == JsonValueKind.Object && recorded.GetProperty("driver").GetString() != current.Driver)
                        return "the graphics driver was updated";
                }
            }
        }
        return null;
    }

    static string ReadShared(string path)
    {
        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using var reader = new StreamReader(file);
        return reader.ReadToEnd();
    }
}

/// The hardware display adapters and their user-mode driver versions, as DXGI reports them: what
/// the game records in the stamp (DisplayDriversJson in src/shader_storage_files.cpp).
static class DisplayDrivers
{
    public sealed record Adapter(string Name, uint Vendor, uint Device, string Driver);

    public static List<Adapter> List()
    {
        var list = new List<Adapter>();
        try
        {
            var factoryId = typeof(IDXGIFactory1).GUID;
            if (CreateDXGIFactory1(ref factoryId, out var factory) < 0 || factory == null) return list;
            try
            {
                var deviceId = new Guid("54ec77fa-1377-44e6-8c32-88fd5f44c84c");  // IDXGIDevice
                for (uint i = 0; factory.EnumAdapters1(i, out var adapter) >= 0 && adapter != null; ++i)
                {
                    try
                    {
                        if (adapter.GetDesc1(out var desc) < 0 || (desc.Flags & 2) != 0) continue;  // software
                        if (adapter.CheckInterfaceSupport(ref deviceId, out long umd) < 0) continue;
                        uint high = (uint)(umd >> 32), low = (uint)umd;
                        list.Add(new(desc.Description, desc.VendorId, desc.DeviceId,
                                     $"{high >> 16}.{high & 0xFFFF}.{low >> 16}.{low & 0xFFFF}"));
                    }
                    finally
                    {
                        Marshal.ReleaseComObject(adapter);
                    }
                }
            }
            finally
            {
                Marshal.ReleaseComObject(factory);
            }
        }
        catch (Exception ex) when (ex is COMException or DllNotFoundException or EntryPointNotFoundException or InvalidCastException)
        {
            Debug.WriteLine($"display drivers: {ex.Message}");
        }
        return list;
    }

    // The GPU the game uses (src/mcla_app.cpp, ChooseGpu, and the rule itself in src/gpu_choice.h; this
    // and that must agree), and from it the API Automatic gives (src/mcla_app.cpp, DecideAutomaticBackend,
    // and the rule itself in src/automatic_api.h; this and that must agree): Vulkan on Intel graphics and
    // on AMD's integrated ones of the RDNA generation, Direct3D 12 on AMD's discrete cards, on AMD's
    // integrated GCN (Vega) ones, on NVIDIA and on anything else.
    //
    // The GPU, in this order: d3d12_adapter where something sets it, with the SDK's meaning (a number is
    // that adapter in the order DXGI lists them, -2 the first software adapter, another negative number
    // the first that isn't software); with mcla_prefer_dedicated_gpu off, the first adapter that isn't
    // software, as before; otherwise the exe's entry in Windows' graphics settings (the value named by the
    // exe's full path in HKCU\Software\Microsoft\DirectX\UserGpuPreferences: GpuPreference=1 power saving,
    // 2 high performance) and, without one or with "let Windows decide" (0), the high-performance GPU,
    // by IDXGIFactory6's ranking. A laptop's integrated GPU is often the first DXGI lists, which is why
    // the first adapter is no longer the default (fork issues #1 and #15, likely cases). An adapter
    // counts only if it isn't software and Direct3D 12 can make a device on it at feature level 11_0.
    //
    // AMD's integrated ones are told by Direct3D 12's own answer (the architecture's UMA flag, from a
    // device made for the adapter), which holds however much memory the BIOS sets aside for the GPU, and
    // RDNA from GCN by the smallest wave the device reports
    // (D3D12_FEATURE_DATA_D3D12_OPTIONS1.WaveLaneCountMin: 32 on RDNA, 64 on GCN). Vulkan also needs
    // vulkan-1.dll (the game also needs a Vulkan device for that GPU, which only it can see). A software
    // adapter (WARP, by -2 or by its number) is "d3d12" as in the game: its vendor is neither AMD's nor
    // Intel's. null when no adapter can be had (or DXGI can't be read, or the number names none). Asked
    // once for each set of options and Windows setting.
    public readonly record struct GpuOptions(int? AdapterOption, bool PreferDedicated, string? Exe)
    {
        public static readonly GpuOptions Default = new(null, true, null);
    }

    public enum GpuWhy { None, UserIndex, UserSoftware, UserFirst, Off, WindowsPowerSaving, WindowsHighPerformance, HighPerformance, FirstListed }

    /// The adapter's number in DXGI's order (-1: none); the one the SDK would take with d3d12_adapter = -1
    /// (-1: none, or not looked for because d3d12_adapter was set); why; whether it becomes d3d12_adapter's
    /// default in the game.
    public readonly record struct GpuChoice(int Index, int FirstIndex, GpuWhy Why, bool SetDefault);

    /// The GPU the game will use, and what Automatic does on it.
    public sealed record ChosenGpu(int Index, string Name, uint Vendor, uint Device, string Api, GpuChoice Choice);

    public static string? AutomaticApi(GpuOptions options) => ChooseGpu(options)?.Api;

    public static string? AutomaticApi(int? adapterOption, bool preferDedicated, string? exe) =>
        AutomaticApi(new GpuOptions(adapterOption, preferDedicated, exe));

    public static ChosenGpu? ChooseGpu(GpuOptions options)
    {
        var preference = options.Exe is null ? WindowsPreferenceNone : WindowsPreferenceFor(options.Exe);
        lock (chosenGpu)
        {
            var key = (options.AdapterOption, options.PreferDedicated, preference);
            if (!chosenGpu.TryGetValue(key, out var gpu)) chosenGpu[key] = gpu = ReadChosenGpu(options, preference);
            return gpu;
        }
    }

    static readonly Dictionary<(int?, bool, int), ChosenGpu?> chosenGpu = [];
    const uint VendorAmd = 0x1002, VendorIntel = 0x8086;
    const uint RdnaWaveLaneCountMin = 32;

    /// The rule, from what is known of the adapter (as AdapterFacts in src/automatic_api.h): Vulkan on
    /// Intel, and on AMD's integrated GPUs (UMA) whose smallest wave is 32 lanes (RDNA; GCN, Vega
    /// included, reports 64: a Ryzen 7 5700G showed the HUD, the menus and parts of the city missing
    /// on Vulkan, fork issue #14). Unknown facts (0) mean Direct3D 12.
    public static bool PrefersVulkan(uint vendor, bool uma, uint waveLaneCountMin) =>
        vendor == VendorIntel || (vendor == VendorAmd && uma && waveLaneCountMin == RdnaWaveLaneCountMin);

    /// What Automatic comes to for such an adapter: "vulkan" when the rule says so and vulkan-1.dll
    /// is there (vulkanAvailable), else "d3d12".
    public static string AutomaticApiFor(uint vendor, bool uma, uint waveLaneCountMin, bool vulkanAvailable) =>
        PrefersVulkan(vendor, uma, waveLaneCountMin) && vulkanAvailable ? "vulkan" : "d3d12";

    /// The registry value's data for the exe, such as "GpuPreference=2;" -> what Windows' graphics settings
    /// say (src/gpu_choice.h, ParseWindowsPreference): 0 let Windows decide, 1 power saving, 2 high
    /// performance; -1 when there is no GpuPreference item or it isn't one of those.
    public const int WindowsPreferenceNone = -1;

    public static int ParseWindowsPreference(string data)
    {
        foreach (var item in data.Split(';'))
        {
            var equals = item.IndexOf('=');
            if (equals < 0 || !item[..equals].Trim(' ', '\t').Equals("GpuPreference", StringComparison.OrdinalIgnoreCase)) continue;
            var value = item[(equals + 1)..].Trim(' ', '\t');
            return value.Length == 1 && value[0] is >= '0' and <= '2' ? value[0] - '0' : WindowsPreferenceNone;
        }
        return WindowsPreferenceNone;
    }

    /// Windows' graphics settings for this exe (Settings > System > Display > Graphics): the value named
    /// by its full path in HKCU\Software\Microsoft\DirectX\UserGpuPreferences, parsed; -1 if there is none.
    public static int WindowsPreferenceFor(string exe)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\DirectX\UserGpuPreferences");
            return key?.GetValue(exe) is string data ? ParseWindowsPreference(data) : WindowsPreferenceNone;
        }
        catch (Exception ex) when (ex is System.Security.SecurityException or UnauthorizedAccessException or IOException or ArgumentException)
        {
            Debug.WriteLine($"graphics settings: {ex.Message}");
            return WindowsPreferenceNone;
        }
    }

    /// The choice of the GPU from what is known of the PC (Choose in src/gpu_choice.h). software[i] is
    /// adapter i's DXGI_ADAPTER_FLAG_SOFTWARE; highPerformance and powerSaving are the adapters' numbers in
    /// IDXGIFactory6's order for DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE and _MINIMUM_POWER (empty without
    /// IDXGIFactory6); usable(i) says whether Direct3D 12 can make a device on adapter i at feature level
    /// 11_0 (only asked of the adapters the rule needs).
    public static GpuChoice ChooseAdapter(bool[] software, int[] highPerformance, int[] powerSaving, int adapterOption,
                                          bool adapterOptionSet, bool preferDedicated, int windowsPreference, Func<int, bool> usable)
    {
        var count = software.Length;
        int FirstUsableHardware()
        {
            for (var i = 0; i < count; ++i)
                if (!software[i] && usable(i)) return i;
            return -1;
        }
        if (adapterOptionSet)
        {
            // The SDK's own reading of the number (src/ui/d3d12/d3d12_provider.cpp).
            if (adapterOption >= 0)
                return new(adapterOption < count && usable(adapterOption) ? adapterOption : -1, -1, GpuWhy.UserIndex, false);
            if (adapterOption == -2)
            {
                for (var i = 0; i < count; ++i)
                    if (software[i] && usable(i)) return new(i, -1, GpuWhy.UserSoftware, false);
                return new(-1, -1, GpuWhy.UserSoftware, false);
            }
            var first = FirstUsableHardware();
            return new(first, first, GpuWhy.UserFirst, false);
        }
        var firstIndex = FirstUsableHardware();
        if (!preferDedicated) return new(firstIndex, firstIndex, firstIndex >= 0 ? GpuWhy.Off : GpuWhy.None, false);
        var powerSavingWanted = windowsPreference == 1;
        foreach (var i in powerSavingWanted ? powerSaving : highPerformance)
        {
            if (i < 0 || i >= count || software[i] || !usable(i)) continue;
            return new(i, firstIndex,
                       powerSavingWanted ? GpuWhy.WindowsPowerSaving : windowsPreference == 2 ? GpuWhy.WindowsHighPerformance : GpuWhy.HighPerformance,
                       true);
        }
        return new(firstIndex, firstIndex, firstIndex >= 0 ? GpuWhy.FirstListed : GpuWhy.None, false);
    }

    static ChosenGpu? ReadChosenGpu(GpuOptions options, int windowsPreference)
    {
        try
        {
            var factoryId = typeof(IDXGIFactory1).GUID;
            if (CreateDXGIFactory1(ref factoryId, out var factory) < 0 || factory == null) return null;
            var adapters = new List<IDXGIAdapter1>();
            try
            {
                var descs = new List<DXGI_ADAPTER_DESC1>();
                var described = new List<bool>();
                for (uint i = 0; factory.EnumAdapters1(i, out var adapter) >= 0 && adapter != null; ++i)
                {
                    adapters.Add(adapter);
                    var ok = adapter.GetDesc1(out var desc) >= 0;
                    descs.Add(ok ? desc : default);
                    described.Add(ok);
                }
                var usableCache = new Dictionary<int, bool>();
                bool Usable(int i)
                {
                    if (i < 0 || i >= adapters.Count || !described[i]) return false;
                    if (!usableCache.TryGetValue(i, out var usable)) usableCache[i] = usable = CanMakeDevice(adapters[i]);
                    return usable;
                }
                var choice = ChooseAdapter([.. descs.Select(d => (d.Flags & 2) != 0)], Rank(factory, 2, descs), Rank(factory, 1, descs),
                                           options.AdapterOption ?? -1, options.AdapterOption.HasValue, options.PreferDedicated, windowsPreference, Usable);
                if (choice.Index < 0) return null;
                var chosen = descs[choice.Index];
                // Only AMD's adapters are asked what they are (an Intel one is Vulkan's whatever it is).
                var (uma, wave) = chosen.VendorId == VendorAmd ? ReadAmdFacts(adapters[choice.Index]) : (false, 0u);
                string api;
                if (!PrefersVulkan(chosen.VendorId, uma, wave)) api = "d3d12";
                else
                {
                    var vulkanAvailable = NativeLibrary.TryLoad("vulkan-1.dll", out var vulkan);
                    if (vulkanAvailable) NativeLibrary.Free(vulkan);
                    api = AutomaticApiFor(chosen.VendorId, uma, wave, vulkanAvailable);
                }
                return new(choice.Index, chosen.Description, chosen.VendorId, chosen.DeviceId, api, choice);
            }
            finally
            {
                foreach (var adapter in adapters) Marshal.ReleaseComObject(adapter);
                Marshal.ReleaseComObject(factory);
            }
        }
        catch (Exception ex) when (ex is COMException or DllNotFoundException or EntryPointNotFoundException or InvalidCastException
                                       or MarshalDirectiveException or SEHException)
        {
            Debug.WriteLine($"automatic api: {ex.Message}");
        }
        return null;
    }

    /// Windows' ranking of the adapters (IDXGIFactory6::EnumAdapterByGpuPreference; 1 is
    /// DXGI_GPU_PREFERENCE_MINIMUM_POWER, 2 DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE) as their numbers in
    /// DXGI's own order, matched by LUID; empty without IDXGIFactory6 (Windows before 10 1803).
    static int[] Rank(IDXGIFactory1 factory, int preference, List<DXGI_ADAPTER_DESC1> descs)
    {
        if (factory is not IDXGIFactory6 factory6) return [];
        var order = new List<int>();
        var adapterId = typeof(IDXGIAdapter1).GUID;
        for (uint i = 0; factory6.EnumAdapterByGpuPreference(i, preference, ref adapterId, out var pointer) >= 0 && pointer != IntPtr.Zero; ++i)
        {
            var listed = (IDXGIAdapter1)Marshal.GetObjectForIUnknown(pointer);
            Marshal.Release(pointer);
            try
            {
                if (listed.GetDesc1(out var desc) < 0) continue;
                var index = descs.FindIndex(d => d.AdapterLuidLowPart == desc.AdapterLuidLowPart && d.AdapterLuidHighPart == desc.AdapterLuidHighPart);
                if (index >= 0) order.Add(index);
            }
            finally
            {
                Marshal.ReleaseComObject(listed);
            }
        }
        return [.. order];
    }

    /// Whether Direct3D 12 can make a device on the adapter at feature level 11_0 (what the SDK requires of
    /// the adapter it takes).
    static bool CanMakeDevice(IDXGIAdapter1 adapter)
    {
        var unknown = Marshal.GetIUnknownForObject(adapter);
        try
        {
            var deviceId = new Guid("189819f1-1db6-4b57-be54-1821339b85f7");  // ID3D12Device
            return D3D12CanCreateDevice(unknown, 0xB000 /* D3D_FEATURE_LEVEL_11_0 */, ref deviceId, IntPtr.Zero) >= 0;
        }
        finally
        {
            Marshal.Release(unknown);
        }
    }

    // What Direct3D 12 says of an adapter's GPU: whether it shares the CPU's memory (an APU), and the
    // smallest wave it runs (0 when the device doesn't say). A device that can't be made at feature
    // level 11_0, or a query that fails, counts as not and 0.
    static (bool Uma, uint WaveLaneCountMin) ReadAmdFacts(IDXGIAdapter1 adapter)
    {
        var unknown = Marshal.GetIUnknownForObject(adapter);
        try
        {
            var deviceId = new Guid("189819f1-1db6-4b57-be54-1821339b85f7");  // ID3D12Device
            if (D3D12CreateDevice(unknown, 0xB000 /* D3D_FEATURE_LEVEL_11_0 */, ref deviceId, out var device) < 0 || device == IntPtr.Zero) return (false, 0);
            // The calls are made through the device's vtable: IUnknown (3 entries: Release is 2),
            // ID3D12Object (GetPrivateData, SetPrivateData, SetPrivateDataInterface, SetName: 4), then
            // ID3D12Device's GetNodeCount, CreateCommandQueue, CreateCommandAllocator,
            // CreateGraphicsPipelineState, CreateComputePipelineState and CreateCommandList (6):
            // CheckFeatureSupport is entry 13 (ID3D12Device is not an ID3D12DeviceChild).
            var vtable = Marshal.ReadIntPtr(device);
            try
            {
                var checkPointer = Marshal.ReadIntPtr(vtable, 13 * IntPtr.Size);
                var check = Marshal.GetDelegateForFunctionPointer<CheckFeatureSupportFn>(checkPointer);
                var data = new D3D12_FEATURE_DATA_ARCHITECTURE();
                const int D3D12_FEATURE_ARCHITECTURE = 1, D3D12_FEATURE_D3D12_OPTIONS1 = 8;
                var uma = check(device, D3D12_FEATURE_ARCHITECTURE, ref data, (uint)Marshal.SizeOf<D3D12_FEATURE_DATA_ARCHITECTURE>()) >= 0 && data.UMA != 0;
                var checkOptions = Marshal.GetDelegateForFunctionPointer<CheckOptions1Fn>(checkPointer);
                var options = new D3D12_FEATURE_DATA_D3D12_OPTIONS1();
                var wave = checkOptions(device, D3D12_FEATURE_D3D12_OPTIONS1, ref options, (uint)Marshal.SizeOf<D3D12_FEATURE_DATA_D3D12_OPTIONS1>()) >= 0 && options.WaveOps != 0
                    ? options.WaveLaneCountMin : 0u;
                return (uma, wave);
            }
            finally
            {
                Marshal.GetDelegateForFunctionPointer<ReleaseFn>(Marshal.ReadIntPtr(vtable, 2 * IntPtr.Size))(device);
            }
        }
        finally
        {
            Marshal.Release(unknown);
        }
    }

    [DllImport("d3d12.dll")]
    static extern int D3D12CreateDevice(IntPtr adapter, int minimumFeatureLevel, ref Guid riid, out IntPtr device);

    // The same function asked only whether a device could be made (the device pointer null).
    [DllImport("d3d12.dll", EntryPoint = "D3D12CreateDevice")]
    static extern int D3D12CanCreateDevice(IntPtr adapter, int minimumFeatureLevel, ref Guid riid, IntPtr device);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int CheckFeatureSupportFn(IntPtr self, int feature, ref D3D12_FEATURE_DATA_ARCHITECTURE data, uint size);

    // The same function (CheckFeatureSupport) with the other feature's data.
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int CheckOptions1Fn(IntPtr self, int feature, ref D3D12_FEATURE_DATA_D3D12_OPTIONS1 data, uint size);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate uint ReleaseFn(IntPtr self);

    [StructLayout(LayoutKind.Sequential)]
    struct D3D12_FEATURE_DATA_ARCHITECTURE
    {
        public uint NodeIndex;
        public int TileBasedRenderer;
        public int UMA;
        public int CacheCoherentUMA;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct D3D12_FEATURE_DATA_D3D12_OPTIONS1
    {
        public int WaveOps;
        public uint WaveLaneCountMin;
        public uint WaveLaneCountMax;
        public uint TotalLaneCount;
        public int ExpandedComputeResourceStates;
        public int Int64ShaderOps;
    }

    [DllImport("dxgi.dll")]
    static extern int CreateDXGIFactory1(ref Guid riid, out IDXGIFactory1? factory);

    // Only the methods called have real signatures; the others hold their vtable slots.
    [ComImport, Guid("770aae78-f26f-4dba-a829-253c83d1b387"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IDXGIFactory1
    {
        void SetPrivateData();
        void SetPrivateDataInterface();
        void GetPrivateData();
        void GetParent();
        void EnumAdapters();
        void MakeWindowAssociation();
        void GetWindowAssociation();
        void CreateSwapChain();
        void CreateSoftwareAdapter();
        [PreserveSig] int EnumAdapters1(uint index, out IDXGIAdapter1? adapter);
    }

    // IDXGIFactory6 with every method of the interfaces it extends (IDXGIObject, IDXGIFactory to IDXGIFactory5)
    // holding its slot; only EnumAdapterByGpuPreference is called.
    [ComImport, Guid("c1b6694f-ff09-44a9-b03c-77900a0a1d17"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IDXGIFactory6
    {
        void SetPrivateData();
        void SetPrivateDataInterface();
        void GetPrivateData();
        void GetParent();
        void EnumAdapters();
        void MakeWindowAssociation();
        void GetWindowAssociation();
        void CreateSwapChain();
        void CreateSoftwareAdapter();
        void EnumAdapters1();
        void IsCurrent();
        void IsWindowedStereoEnabled();
        void CreateSwapChainForHwnd();
        void CreateSwapChainForCoreWindow();
        void GetSharedResourceAdapterLuid();
        void RegisterStereoStatusWindow();
        void RegisterStereoStatusEvent();
        void UnregisterStereoStatus();
        void RegisterOcclusionStatusWindow();
        void RegisterOcclusionStatusEvent();
        void UnregisterOcclusionStatus();
        void CreateSwapChainForComposition();
        void GetCreationFlags();
        void EnumAdapterByLuid();
        void EnumWarpAdapter();
        void CheckFeatureSupport();
        [PreserveSig] int EnumAdapterByGpuPreference(uint adapter, int gpuPreference, ref Guid riid, out IntPtr adapterPointer);
    }

    [ComImport, Guid("29038f61-3839-4626-91fd-086879011a05"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IDXGIAdapter1
    {
        void SetPrivateData();
        void SetPrivateDataInterface();
        void GetPrivateData();
        void GetParent();
        void EnumOutputs();
        void GetDesc();
        [PreserveSig] int CheckInterfaceSupport(ref Guid interfaceName, out long umdVersion);
        [PreserveSig] int GetDesc1(out DXGI_ADAPTER_DESC1 desc);
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DXGI_ADAPTER_DESC1
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
        public uint VendorId;
        public uint DeviceId;
        public uint SubSysId;
        public uint Revision;
        public nuint DedicatedVideoMemory;
        public nuint DedicatedSystemMemory;
        public nuint SharedSystemMemory;
        public uint AdapterLuidLowPart;
        public int AdapterLuidHighPart;
        public uint Flags;
    }
}

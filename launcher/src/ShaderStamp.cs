// Whether shaders should be prepared before Play. After every start and every preparation the
// game records what its storage was loaded with (the stamp, <cache>\shaders\local\545407F8.stamp.json,
// src/shader_prepare.cpp): the shader pack, the API, the resolution scale, the adapter and its
// driver, the files' sizes. This compares it with what the next start will use: the pack in the
// checkout, mcla.toml and the options the launcher passes, the display drivers installed now and
// the files as they are. It reads a few small files and lists the display adapters (DXGI); it
// never starts mcla.exe. Kept free of the launcher's UI, for the setup wizard too.
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;

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

    /// The cache folder the game will use, as the SDK picks it: cache_root, else user_data_root's
    /// cache folder, else Documents\mcla\cache.
    public static string CacheRoot(string arguments, IReadOnlyDictionary<string, string> toml)
    {
        if (Setting("cache_root", arguments, toml) is { Length: > 0 } cache) return cache;
        var user = Setting("user_data_root", arguments, toml) is { Length: > 0 } root
            ? root
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "mcla");
        return Path.Combine(user, "cache");
    }

    public static PrepareCheck Check(string repo, string arguments, IReadOnlyDictionary<string, string> toml, string? skipPack)
    {
        var watch = Stopwatch.StartNew();
        string? reason;
        try
        {
            reason = Reason(repo, arguments, toml, skipPack);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException
                                       or InvalidOperationException or FormatException or KeyNotFoundException)
        {
            // A stamp that can't be read is as good as none.
            reason = "the shader storage's record can't be read";
        }
        return new(reason, watch.ElapsedMilliseconds);
    }

    static string? Reason(string repo, string arguments, IReadOnlyDictionary<string, string> toml, string? skipPack)
    {
        var packFile = ShaderPrepRun.PackFile(repo);
        if (!File.Exists(packFile)) return null;
        var pack = PackHash(packFile);
        // The last automatic preparation failed for this pack: not again at every Play
        // (More > Prepare shaders tries again).
        if (pack != null && pack == skipPack) return null;

        var cache = CacheRoot(arguments, toml);
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

        var stampPack = root.TryGetProperty("pack", out var p) && p.ValueKind == JsonValueKind.String ? p.GetString() : null;
        if (pack != null && stampPack != pack) return "the shader pack changed";

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

        // The API and the resolution scale the next start will use, against what was loaded.
        var api = (Setting("mcla_gpu_backend", arguments, toml) ?? "any").Trim().ToLowerInvariant();
        var stampApi = storage.GetProperty("api").GetString();
        if (api is "d3d12" or "vulkan" && api != stampApi) return "the graphics API changed";
        var scaleText = Setting("resolution_scale", arguments, toml) ?? "1";
        if (int.TryParse(scaleText, NumberStyles.Integer, CultureInfo.InvariantCulture, out var scale) &&
            storage.TryGetProperty("resolution_scale", out var stampScale) && stampScale.ValueKind == JsonValueKind.Array &&
            stampScale.GetArrayLength() > 0 && stampScale[0].GetInt32() != scale)
            return "the resolution changed";

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

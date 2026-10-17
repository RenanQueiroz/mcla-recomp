// What Windows reports about the main display for HDR: whether HDR is on, the
// peak the HDR Calibration app measured (DXGI's IDXGIOutput6::GetDesc1, as
// the game reads it) and the SDR content brightness (DisplayConfig's SDR white
// level). Read once, when the launcher opens; the game reads them itself while
// it runs, and these are only shown next to its HDR settings.
using System.Runtime.InteropServices;

namespace MclaLauncher;

sealed record DisplayHdr(bool HdrOn, float PeakNits, float SdrWhiteNits)
{
    public static readonly Lazy<DisplayHdr?> Main = new(Read);

    public string Describe() =>
        $"Your main display: HDR {(HdrOn ? "on" : "off")}, peak {PeakNits:0} nits" +
        (SdrWhiteNits > 0 ? $", SDR content brightness {SdrWhiteNits:0} nits" : "") +
        " (what AUTO uses).";

    static DisplayHdr? Read()
    {
        try
        {
            var monitor = MonitorFromPoint(0, 0, 1 /* MONITOR_DEFAULTTOPRIMARY */);
            var desc = OutputDesc1(monitor);
            if (desc is not { } d) return null;
            return new DisplayHdr(d.ColorSpace == 12 /* RGB_FULL_G2084_NONE_P2020 */, d.MaxLuminance,
                                  SdrWhiteLevel(d.DeviceName));
        }
        catch (Exception ex) when (ex is COMException or DllNotFoundException or EntryPointNotFoundException
                                       or MarshalDirectiveException or InvalidOperationException)
        {
            return null;
        }
    }

    // --- DXGI, through its vtables (no COM interface declarations needed) ---

    readonly record struct Desc1(string DeviceName, uint ColorSpace, float MaxLuminance);

    [DllImport("dxgi.dll")]
    static extern int CreateDXGIFactory1(ref Guid riid, out IntPtr factory);
    [DllImport("user32.dll")]
    static extern IntPtr MonitorFromPoint(long point, int flags);
    static IntPtr MonitorFromPoint(int x, int y, int flags) => MonitorFromPoint((long)(uint)x | ((long)y << 32), flags);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int EnumDelegate(IntPtr self, uint index, out IntPtr result);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int QueryInterfaceDelegate(IntPtr self, ref Guid riid, out IntPtr result);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    delegate int GetDescDelegate(IntPtr self, IntPtr desc);

    static T Method<T>(IntPtr self, int index) where T : Delegate =>
        Marshal.GetDelegateForFunctionPointer<T>(Marshal.ReadIntPtr(Marshal.ReadIntPtr(self), index * IntPtr.Size));

    static Desc1? OutputDesc1(IntPtr monitor)
    {
        var factoryId = new Guid("770aae78-f26f-4dba-a829-253c83d1b387");  // IDXGIFactory1
        var output6Id = new Guid("068346e8-aaec-4b84-add7-137f513f77a1");  // IDXGIOutput6
        if (CreateDXGIFactory1(ref factoryId, out var factory) < 0) return null;
        var desc = Marshal.AllocHGlobal(256);
        try
        {
            for (uint a = 0; Method<EnumDelegate>(factory, 12)(factory, a, out var adapter) >= 0; ++a)  // EnumAdapters1
            {
                try
                {
                    for (uint o = 0; Method<EnumDelegate>(adapter, 7)(adapter, o, out var output) >= 0; ++o)  // EnumOutputs
                    {
                        try
                        {
                            if (Method<QueryInterfaceDelegate>(output, 0)(output, ref output6Id, out var output6) < 0) continue;
                            try
                            {
                                if (Method<GetDescDelegate>(output6, 27)(output6, desc) < 0) continue;  // GetDesc1
                                // DXGI_OUTPUT_DESC1: DeviceName[32] at 0, Monitor at 88,
                                // ColorSpace at 100, MaxLuminance at 140.
                                if (Marshal.ReadIntPtr(desc, 88) != monitor) continue;
                                return new Desc1(Marshal.PtrToStringUni(desc, 32).TrimEnd('\0'),
                                                 (uint)Marshal.ReadInt32(desc, 100),
                                                 BitConverter.Int32BitsToSingle(Marshal.ReadInt32(desc, 140)));
                            }
                            finally { Marshal.Release(output6); }
                        }
                        finally { Marshal.Release(output); }
                    }
                }
                finally { Marshal.Release(adapter); }
            }
            return null;
        }
        finally
        {
            Marshal.FreeHGlobal(desc);
            Marshal.Release(factory);
        }
    }

    // --- DisplayConfig: the SDR white level of the display with that name ---

    [DllImport("user32.dll")]
    static extern int GetDisplayConfigBufferSizes(uint flags, out uint pathCount, out uint modeCount);
    [DllImport("user32.dll")]
    static extern int QueryDisplayConfig(uint flags, ref uint pathCount, byte[] paths, ref uint modeCount, byte[] modes, IntPtr topology);
    [DllImport("user32.dll")]
    static extern int DisplayConfigGetDeviceInfo(byte[] packet);

    static float SdrWhiteLevel(string deviceName)
    {
        const uint QDC_ONLY_ACTIVE_PATHS = 2;
        const int PathSize = 72, ModeSize = 64;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, out var pathCount, out var modeCount) != 0) return 0;
        var paths = new byte[pathCount * PathSize];
        var modes = new byte[modeCount * ModeSize];
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ref pathCount, paths, ref modeCount, modes, IntPtr.Zero) != 0) return 0;
        for (int i = 0; i < pathCount; ++i)
        {
            // DISPLAYCONFIG_PATH_INFO: source adapter id and id at 0 and 8,
            // target adapter id and id at 20 and 28.
            var source = Header(1 /* GET_SOURCE_NAME */, 84, paths, i * PathSize);
            if (DisplayConfigGetDeviceInfo(source) != 0) continue;
            var name = System.Text.Encoding.Unicode.GetString(source, 20, 64).TrimEnd('\0');
            if (!string.Equals(name, deviceName, StringComparison.OrdinalIgnoreCase)) continue;
            var white = Header(11 /* GET_SDR_WHITE_LEVEL */, 24, paths, i * PathSize + 20);
            if (DisplayConfigGetDeviceInfo(white) != 0) return 0;
            return BitConverter.ToUInt32(white, 20) * 80f / 1000f;
        }
        return 0;
    }

    // A DISPLAYCONFIG_DEVICE_INFO_HEADER (type, size, adapter id, id) for the
    // adapter id and id at `offset` in a path, at the start of a packet.
    static byte[] Header(int type, int size, byte[] paths, int offset)
    {
        var packet = new byte[size];
        BitConverter.GetBytes(type).CopyTo(packet, 0);
        BitConverter.GetBytes(size).CopyTo(packet, 4);
        Array.Copy(paths, offset, packet, 8, 12);
        return packet;
    }
}

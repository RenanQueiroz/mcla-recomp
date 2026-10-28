// Calls the launcher's Automatic-API rule and its choice of the GPU (launcher\src\ShaderStamp.cs, DisplayDrivers)
// through reflection on a built MclaLauncher.dll, three ways:
//   1. with made-up values (vendor, UMA, the smallest wave): the table of launcher\dev\api-rule\api_rule_test.cpp,
//      which tries the game's rule (src\automatic_api.h); the two tables must stay equal;
//   2. with made-up PCs (adapters, Windows' rankings, the registry preference, d3d12_adapter, the off switch): the
//      table of launcher\dev\api-rule\gpu_choice_test.cpp, which tries the game's choice (src\gpu_choice.h); the
//      two tables must stay equal;
//   3. with this PC's adapters: AutomaticApi for each case, against what the caller expects. The exe is the one
//      whose entry in Windows' graphics settings counts (--exe).
//
//   dotnet run -c Release -p:Platform=x64 -- <MclaLauncher.dll> [--mock-only] [--exe C:\path\mcla.exe]
//       [--expect default=d3d12,off=d3d12,-1=d3d12,0=d3d12,1=vulkan,2=d3d12,3=null,-2=d3d12]
// "default" is d3d12_adapter left alone, "off" is mcla_prefer_dedicated_gpu = false, a number is d3d12_adapter = N.
// --mock-only skips the third part (it makes Direct3D 12 devices). Exit code 0 only if every row is as expected.
using System.Reflection;

var dll = args.Length > 0 ? args[0] : throw new Exception("usage: apitest <MclaLauncher.dll> [--expect option=api,...]");
var expectIndex = Array.IndexOf(args, "--expect");
var assembly = Assembly.LoadFrom(dll);
var type = assembly.GetType("MclaLauncher.DisplayDrivers") ?? throw new Exception("DisplayDrivers not found");
const BindingFlags Static = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
var prefers = type.GetMethod("PrefersVulkan", Static) ?? throw new Exception("PrefersVulkan not found");
var apiFor = type.GetMethod("AutomaticApiFor", Static) ?? throw new Exception("AutomaticApiFor not found");
var automatic = type.GetMethod("AutomaticApi", Static, [typeof(int?), typeof(bool), typeof(string)]) ?? throw new Exception("AutomaticApi not found");
var chooseAdapter = type.GetMethod("ChooseAdapter", Static) ?? throw new Exception("ChooseAdapter not found");
var parsePreference = type.GetMethod("ParseWindowsPreference", Static) ?? throw new Exception("ParseWindowsPreference not found");
var chooseGpu = type.GetMethod("ChooseGpu", Static) ?? throw new Exception("ChooseGpu not found");
Console.WriteLine($"launcher: {dll} ({File.GetLastWriteTime(dll):s})");

var failures = 0;

// 1. Made-up values: vendor, UMA, WaveLaneCountMin, whether Automatic comes to Vulkan (with vulkan-1.dll there).
(string What, uint Vendor, bool Uma, uint Wave, bool Vulkan)[] rows =
[
    ("Ryzen 9800X3D Radeon (RDNA 2, UMA, wave 32)", 0x1002, true, 32, true),
    ("Ryzen 7 5700G Vega 8 (GCN, UMA, wave 64)", 0x1002, true, 64, false),
    ("AMD integrated, wave size not answered (0)", 0x1002, true, 0, false),
    ("AMD integrated, an unexpected wave size (16)", 0x1002, true, 16, false),
    ("RX 6600 XT (RDNA 2, discrete, wave 32)", 0x1002, false, 32, false),
    ("RX 580 (GCN, discrete, wave 64)", 0x1002, false, 64, false),
    ("AMD discrete, nothing answered", 0x1002, false, 0, false),
    ("Intel, not asked (UMA no, wave 0)", 0x8086, false, 0, true),
    ("Intel, UMA yes, wave 16", 0x8086, true, 16, true),
    ("Intel, UMA no, wave 32", 0x8086, false, 32, true),
    ("RTX 5080 (not asked)", 0x10DE, false, 0, false),
    ("NVIDIA, UMA yes, wave 32", 0x10DE, true, 32, false),
    ("WARP (Microsoft Basic Render Driver)", 0x1414, false, 0, false),
    ("WARP, UMA yes, wave 32", 0x1414, true, 32, false),
    ("no vendor", 0, false, 0, false),
];
foreach (var (what, vendor, uma, wave, vulkan) in rows)
{
    var rule = (bool)prefers.Invoke(null, [vendor, uma, wave])!;
    var withDriver = (string)apiFor.Invoke(null, [vendor, uma, wave, true])!;
    var withoutDriver = (string)apiFor.Invoke(null, [vendor, uma, wave, false])!;
    // No vulkan-1.dll: Direct3D 12 whatever the rule says.
    var ok = rule == vulkan && withDriver == (vulkan ? "vulkan" : "d3d12") && withoutDriver == "d3d12";
    Console.WriteLine($"{(ok ? "ok  " : "FAIL")}  {what,-48} vendor {vendor:x4} uma {(uma ? 1 : 0)} wave {wave,2} -> {withDriver} (no Vulkan driver: {withoutDriver})");
    if (!ok) failures++;
}

// 2. Made-up PCs: the table of gpu_choice_test.cpp. Adapters: I Intel iGPU, N GeForce, A AMD iGPU, C Radeon card, W WARP (software).
// (what, adapters, high-performance ranking, power-saving ranking, windows preference, prefer dedicated, d3d12_adapter set, option,
//  usable (null: all), expected index, expected first index, expected why, expected set-default)
(string What, string Adapters, int[] High, int[] Power, int Windows, bool Prefer, bool OptionSet, int Option, bool[]? Usable, int Index, int First, string Why, bool SetDefault)[] choices =
[
    ("laptop, Intel listed first, GeForce second: defaults", "INW", [1, 0, 2], [0, 1, 2], -1, true, false, -1, null, 1, 0, "HighPerformance", true),
    ("laptop, Windows already lists the GeForce first", "NIW", [0, 1, 2], [1, 0, 2], -1, true, false, -1, null, 0, 0, "HighPerformance", true),
    ("RDNA iGPU + GeForce, GeForce first (this PC)", "NAW", [0, 1, 2], [1, 0, 2], -1, true, false, -1, null, 0, 0, "HighPerformance", true),
    ("RDNA iGPU listed first + GeForce", "ANW", [1, 0, 2], [0, 1, 2], -1, true, false, -1, null, 1, 0, "HighPerformance", true),
    ("APU's Radeon listed first + Radeon card", "ACW", [1, 0, 2], [0, 1, 2], -1, true, false, -1, null, 1, 0, "HighPerformance", true),
    ("three GPUs: Intel, AMD iGPU, GeForce", "IANW", [2, 1, 0, 3], [0, 1, 2, 3], -1, true, false, -1, null, 2, 0, "HighPerformance", true),
    ("Intel only (and WARP)", "IW", [0, 1], [0, 1], -1, true, false, -1, null, 0, 0, "HighPerformance", true),
    ("GeForce only (and WARP)", "NW", [0, 1], [0, 1], -1, true, false, -1, null, 0, 0, "HighPerformance", true),
    ("RDNA iGPU only (and WARP)", "AW", [0, 1], [0, 1], -1, true, false, -1, null, 0, 0, "HighPerformance", true),
    ("WARP only: nothing to use", "W", [0], [0], -1, true, false, -1, null, -1, -1, "None", false),
    ("no adapter at all", "", [], [], -1, true, false, -1, null, -1, -1, "None", false),
    ("d3d12_adapter = -2 set: the software adapter", "NAW", [0, 1, 2], [1, 0, 2], -1, true, true, -2, null, 2, -1, "UserSoftware", false),
    ("d3d12_adapter = -2 set, no software adapter", "N", [0], [0], -1, true, true, -2, null, -1, -1, "UserSoftware", false),
    ("the ranking lists WARP first: skipped", "IW", [1, 0], [1, 0], -1, true, false, -1, null, 0, 0, "HighPerformance", true),
    ("registry 0 (let Windows decide): high performance", "INW", [1, 0, 2], [0, 1, 2], 0, true, false, -1, null, 1, 0, "HighPerformance", true),
    ("registry 1 (power saving): the Intel iGPU", "INW", [1, 0, 2], [0, 1, 2], 1, true, false, -1, null, 0, 0, "WindowsPowerSaving", true),
    ("registry 2 (high performance): the GeForce", "INW", [1, 0, 2], [0, 1, 2], 2, true, false, -1, null, 1, 0, "WindowsHighPerformance", true),
    ("registry 1 (power saving), RDNA iGPU + GeForce (this PC): the Radeon", "NAW", [0, 1, 2], [1, 0, 2], 1, true, false, -1, null, 1, 0, "WindowsPowerSaving", true),
    ("registry 2 (high performance), RDNA iGPU + GeForce (this PC): the GeForce", "NAW", [0, 1, 2], [1, 0, 2], 2, true, false, -1, null, 0, 0, "WindowsHighPerformance", true),
    ("registry 1 (power saving), one GPU", "IW", [0, 1], [0, 1], 1, true, false, -1, null, 0, 0, "WindowsPowerSaving", true),
    ("registry not understood (-1): high performance", "INW", [1, 0, 2], [0, 1, 2], -1, true, false, -1, null, 1, 0, "HighPerformance", true),
    ("d3d12_adapter = 0 set", "INW", [1, 0, 2], [0, 1, 2], -1, true, true, 0, null, 0, -1, "UserIndex", false),
    ("d3d12_adapter = 1 set", "INW", [1, 0, 2], [0, 1, 2], -1, true, true, 1, null, 1, -1, "UserIndex", false),
    ("d3d12_adapter = 2 set: WARP by its number", "INW", [1, 0, 2], [0, 1, 2], -1, true, true, 2, null, 2, -1, "UserIndex", false),
    ("d3d12_adapter = 7 set: names no adapter", "INW", [1, 0, 2], [0, 1, 2], -1, true, true, 7, null, -1, -1, "UserIndex", false),
    ("d3d12_adapter = 1 set, Direct3D 12 cannot use it", "INW", [1, 0, 2], [0, 1, 2], -1, true, true, 1, [true, false, true], -1, -1, "UserIndex", false),
    ("d3d12_adapter = -1 set: the first, as the SDK", "INW", [1, 0, 2], [0, 1, 2], -1, true, true, -1, null, 0, 0, "UserFirst", false),
    ("d3d12_adapter = 0 set wins over registry 2", "INW", [1, 0, 2], [0, 1, 2], 2, true, true, 0, null, 0, -1, "UserIndex", false),
    ("d3d12_adapter = 0 set wins over registry 1", "NAW", [0, 1, 2], [1, 0, 2], 1, true, true, 0, null, 0, -1, "UserIndex", false),
    ("d3d12_adapter = 0 set wins over the off switch", "INW", [1, 0, 2], [0, 1, 2], -1, false, true, 0, null, 0, -1, "UserIndex", false),
    ("d3d12_adapter = 1 set on this PC: the Radeon", "NAW", [0, 1, 2], [1, 0, 2], -1, true, true, 1, null, 1, -1, "UserIndex", false),
    ("off switch: the Intel iGPU, listed first", "INW", [1, 0, 2], [0, 1, 2], -1, false, false, -1, null, 0, 0, "Off", false),
    ("off switch ignores registry 2", "INW", [1, 0, 2], [0, 1, 2], 2, false, false, -1, null, 0, 0, "Off", false),
    ("off switch, Windows lists the GeForce first", "NIW", [0, 1, 2], [1, 0, 2], -1, false, false, -1, null, 0, 0, "Off", false),
    ("off switch, WARP only", "W", [0], [0], -1, false, false, -1, null, -1, -1, "None", false),
    ("no ranking (no IDXGIFactory6): the first, as the SDK", "INW", [], [], -1, true, false, -1, null, 0, 0, "FirstListed", false),
    ("no ranking, registry 2: still the first", "INW", [], [], 2, true, false, -1, null, 0, 0, "FirstListed", false),
    ("the ranked GeForce cannot make a Direct3D 12 device: the next", "INW", [1, 0, 2], [0, 1, 2], -1, true, false, -1, [true, false, true], 0, 0, "HighPerformance", true),
    ("the first listed cannot make a Direct3D 12 device: it is not 'first'", "INW", [1, 0, 2], [0, 1, 2], -1, true, false, -1, [false, true, true], 1, 1, "HighPerformance", true),
    ("the ranking names an adapter that is not there: skipped", "INW", [9, -1, 1, 0], [0, 1, 2], -1, true, false, -1, null, 1, 0, "HighPerformance", true),
    ("the ranking names no usable adapter: the first, as the SDK", "INW", [2], [2], -1, true, false, -1, null, 0, 0, "FirstListed", false),
];
foreach (var row in choices)
{
    var usable = new Func<int, bool>(i => row.Usable == null || (i >= 0 && i < row.Usable.Length && row.Usable[i]));
    var choice = chooseAdapter.Invoke(null, [row.Adapters.Select(c => c == 'W').ToArray(), row.High, row.Power, row.Option, row.OptionSet, row.Prefer, row.Windows, usable])!;
    var c = choice.GetType();
    var index = (int)c.GetProperty("Index")!.GetValue(choice)!;
    var first = (int)c.GetProperty("FirstIndex")!.GetValue(choice)!;
    var why = c.GetProperty("Why")!.GetValue(choice)!.ToString();
    var setDefault = (bool)c.GetProperty("SetDefault")!.GetValue(choice)!;
    var ok = index == row.Index && first == row.First && why == row.Why && setDefault == row.SetDefault;
    Console.WriteLine($"{(ok ? "ok  " : "FAIL")}  {row.What,-72} -> index {index,2} (first {first,2}) {why,-24} default {(setDefault ? 1 : 0)}");
    if (!ok) { Console.WriteLine($"      expected index {row.Index} (first {row.First}) {row.Why} default {(row.SetDefault ? 1 : 0)}"); failures++; }
}
(string Data, int Expected)[] parses =
[
    ("GpuPreference=2;", 2), ("GpuPreference=1;", 1), ("GpuPreference=0;", 0), ("AppStatus=0;", -1),
    ("AppStatus=0;GpuPreference=2;", 2), ("GpuPreference=1;AppStatus=0;", 1), ("GpuPreference=2", 2), ("gpupreference=1;", 1),
    (" GpuPreference = 2 ; ", 2), ("GpuPreference=3;", -1), ("GpuPreference=12;", -1), ("GpuPreference=;", -1),
    ("GpuPreference=a;", -1), ("GpuPreference;", -1), ("XGpuPreference=2;", -1), ("", -1), (";;;", -1),
    ("GpuPreference=1;GpuPreference=2;", 1),
];
foreach (var (data, expectedPreference) in parses)
{
    var got = (int)parsePreference.Invoke(null, [data])!;
    var ok = got == expectedPreference;
    Console.WriteLine($"{(ok ? "ok  " : "FAIL")}  parse '{data}' -> {got}");
    if (!ok) failures++;
}

// 3. This PC's adapters, as DXGI lists them. The cases: d3d12_adapter left alone, the off switch, and each option.
if (args.Contains("--mock-only"))
{
    Console.WriteLine(failures == 0 ? "apitest: every made-up row as expected" : $"apitest: {failures} FAILURE(S)");
    return failures == 0 ? 0 : 1;
}
var expected = new Dictionary<string, string>();
if (expectIndex >= 0 && expectIndex + 1 < args.Length)
    foreach (var pair in args[expectIndex + 1].Split(',', StringSplitOptions.RemoveEmptyEntries))
    {
        var parts = pair.Split('=');
        expected[parts[0]] = parts[1];
    }
var exeIndex = Array.IndexOf(args, "--exe");
var exe = exeIndex >= 0 && exeIndex + 1 < args.Length ? args[exeIndex + 1] : null;
Console.WriteLine($"exe whose Windows graphics entry counts: {exe ?? "(none)"}");
var cases = new List<(string Name, int? Option, bool Prefer)> { ("default", null, true), ("off", null, false) };
cases.AddRange(new[] { -1, 0, 1, 2, 3, -2 }.Select(o => ($"{o}", (int?)o, true)));
foreach (var (name, option, prefer) in cases)
{
    var result = automatic.Invoke(null, [option, prefer, exe]) as string ?? "null";
    var gpu = chooseGpu.Invoke(null, [Activator.CreateInstance(type.GetNestedType("GpuOptions")!, [option, prefer, exe])]);
    var detail = gpu == null ? "no adapter" : $"adapter {gpu.GetType().GetProperty("Index")!.GetValue(gpu)} '{gpu.GetType().GetProperty("Name")!.GetValue(gpu)}' ({gpu.GetType().GetProperty("Choice")!.GetValue(gpu)})";
    var verdict = expected.TryGetValue(name, out var want) ? (want == result ? "ok  " : "FAIL") : "    ";
    Console.WriteLine($"{verdict}  {(name is "default" or "off" ? name : $"d3d12_adapter={name}")}: {result}  [{detail}]{(verdict == "FAIL" ? $" (expected {want})" : "")}");
    if (verdict == "FAIL") failures++;
}
Console.WriteLine(failures == 0 ? "apitest: every row as expected" : $"apitest: {failures} FAILURE(S)");
return failures == 0 ? 0 : 1;

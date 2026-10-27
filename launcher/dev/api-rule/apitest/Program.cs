// Calls the launcher's Automatic-API rule (launcher\src\ShaderStamp.cs, DisplayDrivers) through reflection on a
// built MclaLauncher.dll, two ways:
//   1. with made-up values (vendor, UMA, the smallest wave): the table of launcher\dev\api-rule\api_rule_test.cpp,
//      which tries the game's rule (src\automatic_api.h); the two tables must stay equal;
//   2. with this PC's adapters: AutomaticApi(d3d12_adapter) for each option, against what the caller expects.
//
//   dotnet run -c Release -p:Platform=x64 -- <MclaLauncher.dll> [--mock-only] [--expect -1=d3d12,0=d3d12,1=vulkan,2=d3d12,3=null,-2=d3d12]
// --mock-only skips the second part (it makes a Direct3D 12 device on an AMD adapter). Exit code 0 only if every row is as expected.
using System.Reflection;

var dll = args.Length > 0 ? args[0] : throw new Exception("usage: apitest <MclaLauncher.dll> [--expect option=api,...]");
var expectIndex = Array.IndexOf(args, "--expect");
var assembly = Assembly.LoadFrom(dll);
var type = assembly.GetType("MclaLauncher.DisplayDrivers") ?? throw new Exception("DisplayDrivers not found");
const BindingFlags Static = BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static;
var prefers = type.GetMethod("PrefersVulkan", Static) ?? throw new Exception("PrefersVulkan not found");
var apiFor = type.GetMethod("AutomaticApiFor", Static) ?? throw new Exception("AutomaticApiFor not found");
var automatic = type.GetMethod("AutomaticApi", Static) ?? throw new Exception("AutomaticApi not found");
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

// 2. This PC's adapters, as DXGI lists them (-1: the first that isn't software, -2: software).
if (args.Contains("--mock-only"))
{
    Console.WriteLine(failures == 0 ? "apitest: every made-up row as expected" : $"apitest: {failures} FAILURE(S)");
    return failures == 0 ? 0 : 1;
}
var expected = new Dictionary<int, string>();
if (expectIndex >= 0 && expectIndex + 1 < args.Length)
    foreach (var pair in args[expectIndex + 1].Split(',', StringSplitOptions.RemoveEmptyEntries))
    {
        var parts = pair.Split('=');
        expected[int.Parse(parts[0])] = parts[1];
    }
foreach (var option in new[] { -1, 0, 1, 2, 3, -2 })
{
    var result = automatic.Invoke(null, [option]) as string ?? "null";
    var verdict = expected.TryGetValue(option, out var want) ? (want == result ? "ok  " : "FAIL") : "    ";
    Console.WriteLine($"{verdict}  d3d12_adapter={option}: {result}{(verdict == "FAIL" ? $" (expected {want})" : "")}");
    if (verdict == "FAIL") failures++;
}
Console.WriteLine(failures == 0 ? "apitest: every row as expected" : $"apitest: {failures} FAILURE(S)");
return failures == 0 ? 0 : 1;

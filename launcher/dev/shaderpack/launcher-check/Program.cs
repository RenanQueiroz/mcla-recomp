// Development only: the launcher's shader logic without its window.
//   check <repo> <mcla.toml> "<options>" [skip pack] [working folder]
//       ShaderStamp.Check (twice, timed), the cache folder and pack it reads, and the display
//       drivers. "\0" in the options is a NUL character (a malformed path). The working folder
//       (default: the current one) is what relative paths in the options are taken against, as
//       the launcher's BuildDir.
//   run <stand-in game> "<options>" <logs folder> [retry]
//       a ShaderPrepRun on a stand-in game: progress, exit code, message, flags
//   stop <stand-in game> "<options>" <logs folder> <ms>
//       the same, ended with Stop() after <ms>: exit code, StopRequested
//   settings <mcla.toml> <refresh Hz> [key=value ...]
//       the Graphics rows the settings page shows for the file (its frame rate row depends on frame
//       generation and the refresh rate), after writing the key=values as the page does, and the file
//   --...   (as the stand-in game) prints the lines of the file FAKEMCLA_SCRIPT ("sleep <ms>"
//       waits, "exit <code>" ends), for the run mode
namespace MclaLauncher;

static class Program
{
    static int Main(string[] args)
    {
        if (args.Length > 0 && args[0].StartsWith("--")) return Stand_in();
        if (args.Length >= 3 && args[0] == "settings") return Settings(args);
        if (args.Length >= 4 && args[0] == "check")
        {
            var toml = GameSettingsFile.Read(args[2]);
            var options = args[3].Replace("\\0", "\0");
            var skip = args.Length > 4 && args[4] != "-" ? args[4] : null;
            var folder = args.Length > 5 ? args[5] : Directory.GetCurrentDirectory();
            var first = ShaderStamp.Check(args[1], folder, options, toml, skip);
            var again = ShaderStamp.Check(args[1], folder, options, toml, skip);
            Console.WriteLine($"reason: {first.Reason ?? "(none)"}");
            Console.WriteLine($"ms: first {first.Milliseconds}, again {again.Milliseconds}");
            try
            {
                Console.WriteLine($"cache: {ShaderStamp.CacheRoot(options, folder)}");
                Console.WriteLine($"pack: {ShaderStamp.PackFile(args[1], options, toml, folder)}");
            }
            catch (Exception ex) when (ex is ArgumentException or NotSupportedException)
            {
                Console.WriteLine($"cache/pack: {ex.GetType().Name}");
            }
            foreach (var a in DisplayDrivers.List())
                Console.WriteLine($"adapter: {a.Name} vendor {a.Vendor} device {a.Device} driver {a.Driver}");
            return 0;
        }
        if (args.Length >= 4 && args[0] == "run")
        {
            var run = new ShaderPrepRun(args[1], args[2], p => Console.WriteLine($"progress: {p.Phase} {p.Done}/{p.Total} {p.Describe()}"),
                                        args[3], reason: "test", retry: args.Length > 4);
            var exit = run.Completion.Result;
            Console.WriteLine($"exit {exit}: {ShaderPrepRun.Describe(exit, run.ErrorMessage)}; created {run.PipelinesCreated}; " +
                              $"import disabled {run.ImportDisabled}; summary {run.Summary}; log {run.LogFile}");
            return 0;
        }
        if (args.Length >= 5 && args[0] == "stop")
        {
            var run = new ShaderPrepRun(args[1], args[2], p => Console.WriteLine($"progress: {p.Phase} {p.Done}/{p.Total}"),
                                        args[3], reason: "test");
            Thread.Sleep(int.Parse(args[4]));
            run.Stop();
            var exit = run.Completion.Result;
            Console.WriteLine($"exit {exit}; stop requested {run.StopRequested}; transient {ShaderPrepRun.IsTransient(exit)}");
            return 0;
        }
        Console.Error.WriteLine("usage: see Program.cs");
        return 2;
    }

    static int Settings(string[] args)
    {
        var refresh = double.Parse(args[2], System.Globalization.CultureInfo.InvariantCulture);
        // The GPU vendor for the low latency setting: LAUNCHER_CHECK_VENDOR (hex, 10DE NVIDIA, 1002 AMD),
        // else the first adapter's.
        var vendor = Environment.GetEnvironmentVariable("LAUNCHER_CHECK_VENDOR") is { Length: > 0 } text
            ? Convert.ToUInt32(text, 16) : GpuVendor.First.Value;
        var set = new Dictionary<string, object>();
        foreach (var pair in args.Skip(3))
        {
            var at = pair.IndexOf('=');
            set[pair[..at]] = SettingValues.Parse(pair[(at + 1)..]);
        }
        // One write per key, as the page does (SetGameSetting).
        foreach (var (key, value) in set) GameSettingsFile.Set(args[1], new Dictionary<string, object> { [key] = value });
        var file = GameSettingsFile.Read(args[1]);
        Console.WriteLine($"half display cap at {refresh} Hz: {GameSettings.HalfDisplayCap(refresh)}");
        foreach (var setting in GameSettings.GraphicsFor(file, refresh, vendor))
        {
            var (choices, index) = setting.Show(file);
            Console.WriteLine($"{setting.Header} [{setting.Key}]: {choices[index].Label} <- {string.Join(" | ", choices.Select(c => c.Label))}");
        }
        Console.WriteLine("--- file");
        Console.Write(File.Exists(args[1]) ? File.ReadAllText(args[1]) : "(none)" + Environment.NewLine);
        return 0;
    }

    static int Stand_in()
    {
        var script = Environment.GetEnvironmentVariable("FAKEMCLA_SCRIPT") ?? "";
        Console.Error.WriteLine("args: " + Environment.CommandLine);
        foreach (var line in File.Exists(script) ? File.ReadAllLines(script) : [])
        {
            if (line.StartsWith("sleep ")) { Thread.Sleep(int.Parse(line[6..])); continue; }
            if (line.StartsWith("exit ")) return int.Parse(line[5..]);
            Console.WriteLine(line);
        }
        return 0;
    }
}

// Development only: the launcher's shader logic without its window.
//   check <repo> <mcla.toml> "<options>" [skip pack]
//       ShaderStamp.Check (twice, timed), the cache folder it reads, and the display drivers
//   run <stand-in game> "<options>" <logs folder> [retry]
//       a ShaderPrepRun on a stand-in game: progress, exit code, message, flags
//   --...   (as the stand-in game) prints the lines of the file FAKEMCLA_SCRIPT ("sleep <ms>"
//       waits, "exit <code>" ends), for the run mode
namespace MclaLauncher;

static class Program
{
    static int Main(string[] args)
    {
        if (args.Length > 0 && args[0].StartsWith("--")) return Stand_in();
        if (args.Length >= 4 && args[0] == "check")
        {
            var toml = GameSettingsFile.Read(args[2]);
            var skip = args.Length > 4 ? args[4] : null;
            var first = ShaderStamp.Check(args[1], args[3], toml, skip);
            var again = ShaderStamp.Check(args[1], args[3], toml, skip);
            Console.WriteLine($"reason: {first.Reason ?? "(none)"}");
            Console.WriteLine($"ms: first {first.Milliseconds}, again {again.Milliseconds}");
            Console.WriteLine($"cache: {ShaderStamp.CacheRoot(args[3], toml)}");
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
        Console.Error.WriteLine("usage: see Program.cs");
        return 2;
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

// Checks of the updates of a wizard install (phase 5). `update-status` and `update-run` run in Installed mode: from a
// copy of this program with an install.json beside it (the pointer), as the launcher's exe has after the setup.
using System.Diagnostics;
using MclaLauncher.Setup;

namespace MclaLauncher;

static partial class Program
{
    static async Task UpdateStatus()
    {
        Check("this run is in Installed mode", Paths.Mode == LaunchMode.Installed);
        var channel = InstallChannel.Read(Paths.InstallRoot!);
        var status = await Repo.GetStatusAsync(fetch: true);
        var target = status.Target;
        Console.WriteLine($"channel {channel}; HEAD {status.Head[..8]}; target {target?.Label} {target?.Commit[..8]}; incoming {status.Incoming.Count}; "
                          + $"patches changed {status.PatchesChanged}; switch back {target?.SwitchBack}; launcher outdated {status.LauncherOutdated}; "
                          + $"update available {status.UpdateAvailable}; build current {status.BuildCurrent}; fetch error {status.FetchError ?? "none"}");
        foreach (var commit in status.Incoming.Take(5)) Console.WriteLine($"  {commit.Hash} {commit.Date} {commit.Subject}");
    }

    /// What the launcher's "Update and rebuild" does in Installed mode: resolve, prepare (downloads), run build-game.ps1 -Ref,
    /// finish (the record, the old tools and bundle).
    static async Task<int> UpdateRun()
    {
        Check("this run is in Installed mode", Paths.Mode == LaunchMode.Installed);
        var status = await Repo.GetStatusAsync(fetch: true);
        if (status.Target is not { } target) { Console.WriteLine($"nothing to update to ({status.FetchError ?? "no target"})"); return 1; }
        if (!status.UpdateAvailable) { Console.WriteLine($"already at {target.Label}"); return 0; }
        var layout = new InstallLayout(Paths.InstallRoot!);
        using var session = await UpdateEngine.PrepareAsync(layout, target, line => Console.WriteLine($"[update] {line}"),
                                                           new Progress<string>(text => Console.WriteLine($"[update] {text}")), CancellationToken.None);
        Console.WriteLine($"build-game.ps1 {string.Join(' ', session.Options.Arguments)}");
        var run = new BuildRun(BuildKind.Build, line =>
        {
            if (line.StartsWith("==> ") || line.StartsWith("SDK bundle") || line.StartsWith("unpacking") || line.Contains("patches changed") || line.StartsWith("Done in")
                || line.StartsWith("already") || line.Contains("is not an ancestor") || line.StartsWith("FAILED") || line.StartsWith("note:") || line.Contains("ancestor"))
                Console.WriteLine($"  {line}");
        }, session.Options);
        var exit = await run.Completion;
        if (exit == 0) UpdateEngine.Finish(layout, AppContext.BaseDirectory, session); else UpdateEngine.KeepOptionsAfterFailure(layout, session);
        Console.WriteLine($"build exit code {exit}; the game changed: {session.GameChanged}; log {run.LogFile}");
        return exit;
    }

    /// mcla.toml keeps what the player set when the setup takes its own key out (N2 of the phase 4 verification).
    static void SettingsRemove(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var file = Path.Combine(scratch, "mcla.toml");
        File.WriteAllLines(file, ["# the player's file", "resolution_scale = 3", "game_data_root = \"D:/Games/MCLA\"", "mcla_fps = 60  # kept", "",
                                  "[log]", "level = \"info\"", "game_data_root = \"not top level\""]);
        GameSettingsFile.Remove(file, "game_data_root");
        var text = File.ReadAllText(file);
        Check("the top-level key is gone", !text.Contains("D:/Games/MCLA"));
        Check("the other keys, comments and tables are kept", text.Contains("# the player's file") && text.Contains("resolution_scale = 3") && text.Contains("mcla_fps = 60  # kept") && text.Contains("[log]") && text.Contains("level = \"info\""));
        Check("a key of a table with the same name is kept", text.Contains("not top level"));
        GameSettingsFile.Remove(file, "game_data_root", "nothing");
        Check("removing what is not there changes nothing", File.ReadAllText(file) == text);
        GameSettingsFile.Remove(Path.Combine(scratch, "missing.toml"), "game_data_root");
        Check("a file that is not there is left alone", !File.Exists(Path.Combine(scratch, "missing.toml")));
    }

    /// The swap script with a "new launcher" that exits at once (whoami.exe under the launcher's name): the old folder
    /// must be put back, started again, and the failed one kept as <folder>.failed.
    static async Task SwapRollback(string scratch)
    {
        SafeFolder.Delete(scratch);
        var exe = Path.Combine(scratch, "app");
        var staging = exe + ".new";
        Directory.CreateDirectory(exe);
        Directory.CreateDirectory(staging);
        var whoami = Path.Combine(Environment.SystemDirectory, "whoami.exe");
        // The old launcher: stays alive a few seconds and leaves a note that it was started again.
        File.Copy(whoami, Path.Combine(exe, LauncherSwap.ExeName));
        File.WriteAllText(Path.Combine(exe, "old.txt"), "old");
        File.Copy(whoami, Path.Combine(staging, LauncherSwap.ExeName));
        File.WriteAllText(Path.Combine(staging, "new.txt"), "new");
        var layout = new InstallLayout(Path.Combine(scratch, "install"));
        // A process to wait for: one that ends in a moment.
        var waitFor = Process.Start(new ProcessStartInfo("ping.exe", "-n 2 127.0.0.1") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!;
        var log = LauncherSwap.StartSwap(layout, staging, exe, waitFor.Id, secondsToWait: 6);
        for (var i = 0; i < 80 && !(File.Exists(log) && File.ReadAllText(log).Contains("old launcher is back")); i++) await Task.Delay(500);
        await Task.Delay(1500);
        var text = File.Exists(log) ? File.ReadAllText(log) : "";
        Console.WriteLine(text);
        Check("the script saw the new launcher not start", text.Contains("did not start"));
        Check("the old folder is back in place", File.Exists(Path.Combine(exe, "old.txt")) && File.Exists(Path.Combine(exe, LauncherSwap.ExeName)));
        Check("the failed one is kept aside", File.Exists(Path.Combine(exe + ".failed", "new.txt")));
        Check("nothing is left as .old or .new", !Directory.Exists(exe + ".old") && !Directory.Exists(staging));
    }
}

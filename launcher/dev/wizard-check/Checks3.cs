// Checks of the updates of a wizard install (phase 5). `update-status`, `update-run`, `rebuild` and `channel` run in Installed
// mode: from a copy of this program with an install.json beside it (the pointer), as the launcher's exe has after the setup.
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
        var pending = InstallRecord.Load(Path.Combine(Paths.InstallRoot!, InstallLayout.RecordName))?.Pending;
        Console.WriteLine($"channel {channel}; HEAD {status.Head[..8]}; target {target?.Label} {target?.Commit[..8]}; incoming {status.Incoming.Count}; "
                          + $"patches changed {status.PatchesChanged}; switch back {target?.SwitchBack}; launcher outdated {status.LauncherOutdated}; "
                          + $"update available {status.UpdateAvailable}; build current {status.BuildCurrent}; pending {(pending == null ? "none" : pending.Tag + " " + pending.Commit[..8])}; "
                          + $"fetch error {status.FetchError ?? "none"}");
        foreach (var commit in status.Incoming.Take(5)) Console.WriteLine($"  {commit.Hash} {commit.Date} {commit.Subject}");
    }

    static string Brief(string line) =>
        line.StartsWith("==> ") || line.StartsWith("SDK bundle") || line.StartsWith("unpacking") || line.Contains("patches changed") || line.StartsWith("Done in")
        || line.StartsWith("already") || line.Contains("ancestor") || line.StartsWith("FAILED") || line.StartsWith("note:") || line.Contains("CMake")
        || line.Contains("Configuring") || line.Contains("Generating") ? $"  {line}" : "";

    /// What the launcher's "Update and rebuild" does in Installed mode: resolve, prepare (downloads, the record, the tools, the move,
    /// the options), run the build, finish. `killAfter`: seconds after the build starts the process ends the hard way (a stop in
    /// the middle of an update: the job object ends the build with it).
    static async Task<int> UpdateRun(int killAfter)
    {
        Check("this run is in Installed mode", Paths.Mode == LaunchMode.Installed);
        ChildJob.Enabled = true;
        var status = await Repo.GetStatusAsync(fetch: true);
        if (status.Target is not { } target) { Console.WriteLine($"nothing to update to ({status.FetchError ?? "no target"})"); return 1; }
        if (!status.UpdateAvailable) { Console.WriteLine($"already at {target.Label}"); return 0; }
        var layout = new InstallLayout(Paths.InstallRoot!);
        using var session = await UpdateEngine.PrepareAsync(layout, target, line => Console.WriteLine($"[update] {line}"),
                                                           new Progress<string>(text => Console.WriteLine($"[update] {text}")), CancellationToken.None);
        Console.WriteLine($"build-game.ps1 {string.Join(' ', session.Options.Arguments)}");
        var run = new BuildRun(BuildKind.Build, line => { if (Brief(line) is { Length: > 0 } text) Console.WriteLine(text); }, session.Options);
        if (killAfter > 0)
        {
            await Task.Delay(TimeSpan.FromSeconds(killAfter));
            Console.WriteLine($"the process ends now, {killAfter} s into the build");
            Process.GetCurrentProcess().Kill();
        }
        var exit = await run.Completion;
        if (exit == 0) UpdateEngine.Finish(layout, AppContext.BaseDirectory, session);
        Console.WriteLine($"build exit code {exit}; the game changed: {session.GameChanged}; log {run.LogFile}");
        return exit;
    }

    /// The launcher's plain Rebuild in Installed mode: the folder's lock, the build as the last options say, and the finish of an
    /// update that was pending.
    static async Task<int> Rebuild()
    {
        Check("this run is in Installed mode", Paths.Mode == LaunchMode.Installed);
        ChildJob.Enabled = true;
        var layout = new InstallLayout(Paths.InstallRoot!);
        await UpdateEngine.RepairPendingAsync(layout);
        using var folderLock = InstallLock.Acquire(layout.Root);
        var run = new BuildRun(BuildKind.Build, line => { if (Brief(line) is { Length: > 0 } text) Console.WriteLine(text); });
        var exit = await run.Completion;
        if (exit == 0) UpdateEngine.CompletePending(layout, AppContext.BaseDirectory);
        var record = InstallRecord.Load(layout.RecordFile);
        Console.WriteLine($"rebuild exit code {exit}; record: {record?.ReleaseTag} {record?.ReleaseCommit?[..8]}; pending {(record?.Pending == null ? "none" : record.Pending.Tag)}; log {run.LogFile}");
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
}

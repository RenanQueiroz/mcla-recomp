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
        await UpdateEngine.RepairPendingAsync(layout, AppContext.BaseDirectory);
        using var folderLock = InstallLock.Acquire(layout.Root);
        var run = new BuildRun(BuildKind.Build, line => { if (Brief(line) is { Length: > 0 } text) Console.WriteLine(text); });
        var exit = await run.Completion;
        if (exit == 0) UpdateEngine.CompletePending(layout, AppContext.BaseDirectory);
        var record = InstallRecord.Load(layout.RecordFile);
        Console.WriteLine($"rebuild exit code {exit}; record: {record?.ReleaseTag} {record?.ReleaseCommit?[..8]}; pending {(record?.Pending == null ? "none" : record.Pending.Tag)}; log {run.LogFile}");
        return exit;
    }

    /// H2 of the phase 6 review: the install folder is never the folder of the running launcher or of the installer's default launcher
    /// folder (%LOCALAPPDATA%\Programs\MCLA), nor inside or around one, also through a junction.
    static void LauncherFolder(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var here = Path.GetFullPath(AppContext.BaseDirectory).TrimEnd('\\');
        var parent = Path.GetDirectoryName(here)!;
        var programs = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs");
        var installer = Path.Combine(programs, "MCLA");
        bool Refused(string path) => PathBudget.Problem(path, out _) is { } text && text.Contains("launcher", StringComparison.OrdinalIgnoreCase);
        Check("the launcher's own folder is refused", Refused(here), PathBudget.Problem(here, out _) ?? "accepted");
        Check("a folder inside it is refused", Refused(Path.Combine(here, "MCLA")));
        Check("a folder that contains it is refused", Refused(parent));
        Check("the same in other letters", Refused(here.ToUpperInvariant()) && Refused(parent.ToLowerInvariant()));
        Check("the installer's default folder is refused", Refused(installer));
        Check("a folder inside it is refused (the game is not kept where the launcher is swapped)", Refused(Path.Combine(installer, "game")));
        Check("a folder that contains it is refused", Refused(programs) && Refused(Path.GetDirectoryName(programs)!));
        Check("an unrelated folder is not refused for it", PathBudget.LauncherFolderProblem(Path.Combine(scratch, "fine")) == null);
        Check("a folder beside the launcher's is not refused for it", PathBudget.LauncherFolderProblem(here + "-game") == null);

        // Junctions: the path that is typed is not the folder that is meant.
        void Junction(string link, string target)
        {
            var p = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", $"/c mklink /J \"{link}\" \"{target}\"")
                { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!;
            p.StandardOutput.ReadToEnd();
            p.WaitForExit();
        }
        var toHere = Path.Combine(scratch, "jhere");
        var toParent = Path.Combine(scratch, "jparent");
        var toPrograms = Path.Combine(scratch, "jprograms");
        Junction(toHere, here);
        Junction(toParent, parent);
        Directory.CreateDirectory(programs);
        Junction(toPrograms, programs);
        try
        {
            Check("the junctions exist", Directory.Exists(toHere) && Directory.Exists(toParent) && Directory.Exists(toPrograms));
            Check("a junction to the launcher's folder is refused", Refused(toHere));
            Check("a folder below a junction to it is refused", Refused(Path.Combine(toHere, "game")));
            Check("a junction to a folder that contains it is refused", Refused(toParent));
            Check("a folder below a junction to Programs, where MCLA would be, is refused", Refused(Path.Combine(toPrograms, "MCLA", "game")));
            Check("a junction to Programs itself (it contains the installer's folder) is refused", Refused(toPrograms));
            Check("a folder on the other side of a junction that is not around the launcher is not refused for it", PathBudget.LauncherFolderProblem(Path.Combine(toPrograms, "Other", "game")) == null);
        }
        finally
        {
            foreach (var link in new[] { toHere, toParent, toPrograms })
                if (Directory.Exists(link)) Directory.Delete(link);   // the link only, never what it points to
        }
        Check("the launcher's folder is still there after the junctions went", Directory.Exists(here) && Directory.Exists(parent));
        SafeFolder.Delete(scratch);
    }

    /// The tools record (P1-P3 of the phase 5 verification): the tools in use are never superseded on account of a record that is
    /// lost or cut short; an update's tools are unpacked where no build looks and moved into place at activation; the settings
    /// restore leaves what an update kept in kept\\.
    static async Task ToolsRecord(string scratch)
    {
        SafeFolder.Delete(scratch);
        var tools = Path.Combine(scratch, "tools");
        Directory.CreateDirectory(tools);
        string[] folders = ["llvm-mingw-20250709-ucrt-x86_64", "cmake-4.4.4-windows-x86_64", "ninja-1.13.2", "ninja-1.13.3", "mingit-2.56.0.2"];
        foreach (var f in folders)
        {
            Directory.CreateDirectory(Path.Combine(tools, f));
            File.WriteAllText(Path.Combine(tools, f, ".mcla-checked"), "sha-" + f + "\n");
        }
        Directory.CreateDirectory(Path.Combine(tools, "unrelated-folder"));   // no marker: never ours
        var record = Path.Combine(tools, ".mcla-tools.txt");
        static string Names(IEnumerable<string> paths) => string.Join(", ", paths.Select(Path.GetFileName).OrderBy(n => n));

        // A record that is missing: nothing is superseded.
        Check("no record: nothing is superseded", ToolInstaller.Superseded(tools).Count == 0, Names(ToolInstaller.Superseded(tools)));
        File.WriteAllText(record, "");
        Check("an empty record: nothing is superseded", ToolInstaller.Superseded(tools).Count == 0);
        // A record cut short in the middle of a line (a stop inside a rewrite that was not atomic).
        File.WriteAllText(record, "llvm-mingw\t" + folders[0] + "\tsha\ncmake\t" + folders[1] + "\tsha\nninja\tninja-1.1");
        Check("a record cut short names only whole lines: nothing is superseded", ToolInstaller.Superseded(tools).Count == 0, Names(ToolInstaller.Superseded(tools)));
        File.WriteAllText(record, "garbage without tabs\n\t\t\n");
        Check("a damaged record: nothing is superseded", ToolInstaller.Superseded(tools).Count == 0);
        // A record that does not name ninja at all: both ninja folders stay.
        File.WriteAllText(record, "llvm-mingw\t" + folders[0] + "\tsha\ncmake\t" + folders[1] + "\tsha\nmingit\t" + folders[4] + "\tsha\n");
        Check("a record without ninja leaves both ninja folders", ToolInstaller.Superseded(tools).Count == 0, Names(ToolInstaller.Superseded(tools)));
        // A whole record: the one it replaced is superseded, the others are not.
        File.WriteAllText(record, "llvm-mingw\t" + folders[0] + "\tsha\ncmake\t" + folders[1] + "\tsha\nninja\t" + folders[3] + "\tsha\nmingit\t" + folders[4] + "\tsha\n");
        Check("a whole record supersedes only the replaced ninja", Names(ToolInstaller.Superseded(tools)) == "ninja-1.13.2", Names(ToolInstaller.Superseded(tools)));
        // A temporary folder of an unpacking that was cut short goes whatever the record says; the unmarked folder never.
        Directory.CreateDirectory(Path.Combine(tools, ".u-ninja"));
        File.Delete(record);
        Check("a temporary folder is listed even without a record, the unmarked one never", Names(ToolInstaller.Superseded(tools)) == ".u-ninja", Names(ToolInstaller.Superseded(tools)));
        SafeFolder.Delete(Path.Combine(tools, ".u-ninja"));

        // The record is replaced whole (a leftover temporary file does not matter) and keeps the other tools.
        File.WriteAllText(record + ".tmp", "leftover from a stop");
        ToolInstaller.Activate(tools, "ninja", folders[2], "shaA");
        ToolInstaller.Activate(tools, "mingit", folders[4], "shaB");
        var lines = File.ReadAllLines(record);
        Check("the record is written whole and has no leftover", lines.Length == 2 && !File.Exists(record + ".tmp") && lines.Any(l => l == "ninja\t" + folders[2] + "\tshaA"), string.Join(" | ", lines));
        ToolInstaller.Activate(tools, "ninja", folders[3], "shaC");
        lines = File.ReadAllLines(record);
        Check("activating a tool replaces its line only", lines.Length == 2 && lines.Contains("ninja\t" + folders[3] + "\tshaC") && lines.Contains("mingit\t" + folders[4] + "\tshaB"), string.Join(" | ", lines));
        Check("then the old ninja is the one superseded", Names(ToolInstaller.Superseded(tools)) == "ninja-1.13.2", Names(ToolInstaller.Superseded(tools)));

        // Staging: an update's tool is unpacked where no build looks, and moved into place at activation.
        var zip = Path.Combine(scratch, "ninja.zip");
        using (var z = System.IO.Compression.ZipFile.Open(zip, System.IO.Compression.ZipArchiveMode.Create))
            using (var w = new StreamWriter(z.CreateEntry("ninja.exe").Open())) w.Write("x");
        var log = new List<string>();
        var staged = await ToolInstaller.InstallAsync(tools, "ninja", "1.14.0", zip, "shaD", log.Add, new Progress<(int, int)>(), CancellationToken.None, record: false);
        Check("an update's tool is unpacked into tools\\.staged", Path.GetDirectoryName(staged)!.EndsWith(ToolInstaller.StagingFolder) && File.Exists(Path.Combine(staged, "ninja.exe")), staged);
        Check("no folder of tools\\ matches it (env.ps1 looks for ninja-*)", !Directory.Exists(Path.Combine(tools, "ninja-1.14.0")));
        Check("it is staged and not in use", ToolInstaller.IsStaged(tools, "ninja-1.14.0", "shaD") && !ToolInstaller.IsInstalled(tools, "ninja", "shaD")
                                              && ToolInstaller.FolderOf(tools, "ninja")!.EndsWith("ninja-1.13.3"));
        var again = await ToolInstaller.InstallAsync(tools, "ninja", "1.14.0", zip, "shaD", log.Add, new Progress<(int, int)>(), CancellationToken.None, record: false);
        Check("a staged tool is not unpacked again", again == staged && log.Any(l => l.Contains("already unpacked")));
        ToolInstaller.Activate(tools, "ninja", "ninja-1.14.0", "shaD");
        Check("activation moves it into tools\\ and records it", Directory.Exists(Path.Combine(tools, "ninja-1.14.0")) && !Directory.Exists(staged)
                                                             && ToolInstaller.IsInstalled(tools, "ninja", "shaD"));
        Check("and the one before it is now the superseded one", Names(ToolInstaller.Superseded(tools)) == "ninja-1.13.2, ninja-1.13.3", Names(ToolInstaller.Superseded(tools)));
        // A repackaged tool that unpacks into a folder of the same name: the one in place is set aside, then the new one moves in.
        var zip2 = Path.Combine(scratch, "ninja2.zip");
        using (var z = System.IO.Compression.ZipFile.Open(zip2, System.IO.Compression.ZipArchiveMode.Create))
            using (var w = new StreamWriter(z.CreateEntry("ninja.exe").Open())) w.Write("y");
        await ToolInstaller.InstallAsync(tools, "ninja", "1.14.0", zip2, "shaE", log.Add, new Progress<(int, int)>(), CancellationToken.None, record: false);
        Check("a same-named folder in use is not touched by the unpacking", File.ReadAllText(Path.Combine(tools, "ninja-1.14.0", "ninja.exe")) == "x");
        ToolInstaller.Activate(tools, "ninja", "ninja-1.14.0", "shaE");
        Check("activation of a repackaged tool switches it and sets the old one aside as a temporary folder",
              File.ReadAllText(Path.Combine(tools, "ninja-1.14.0", "ninja.exe")) == "y" && Directory.Exists(Path.Combine(tools, ".u-old-ninja-1.14.0")));
        ToolInstaller.RemoveStaged(tools);
        Check("nothing is left staged", !Directory.Exists(Path.Combine(tools, ToolInstaller.StagingFolder)));

        // The settings restore (a re-run of the setup) removes what it restored and nothing else in kept\.
        var layout = new InstallLayout(Path.Combine(scratch, "install"));
        var toml = Path.Combine(scratch, "put-back", "mcla.toml");
        Directory.CreateDirectory(Path.Combine(layout.Kept, "codegen-edits"));
        File.WriteAllText(Path.Combine(layout.Kept, "mcla.toml"), "mine");
        File.WriteAllText(Path.Combine(layout.Kept, "codegen-edits", "mcla_manifest.toml.20260101-000000"), "my edit");
        var restored = new List<string>();
        SetupEngine.RestoreKept(layout, [(toml, "mcla.toml")], restored.Add);
        Check("the restored file is back and gone from kept\\", File.ReadAllText(toml) == "mine" && !File.Exists(Path.Combine(layout.Kept, "mcla.toml")) && restored.SequenceEqual(["mcla.toml"]));
        Check("the saved edit of an update is still in kept\\codegen-edits", File.ReadAllText(Path.Combine(layout.Kept, "codegen-edits", "mcla_manifest.toml.20260101-000000")) == "my edit");
        SafeFolder.Delete(Path.Combine(layout.Kept, "codegen-edits"));
        File.WriteAllText(Path.Combine(layout.Kept, "settings.json"), "{}");
        SetupEngine.RestoreKept(layout, [(Path.Combine(scratch, "put-back", "settings.json"), "settings.json")], restored.Add);
        Check("with nothing else in it, kept\\ goes", !Directory.Exists(layout.Kept));
        SafeFolder.Delete(scratch);
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

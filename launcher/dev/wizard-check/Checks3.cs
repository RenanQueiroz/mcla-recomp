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

        // R1: a chain of junctions, a junction to a drive root, a substituted drive (on either side).
        var chain1 = Path.Combine(scratch, "jchain1");
        var chain2 = Path.Combine(scratch, "jchain2");
        var toRoot = Path.Combine(scratch, "jroot");
        var driveRoot = Path.GetPathRoot(programs)!;
        Junction(chain2, programs);
        Junction(chain1, chain2);
        Junction(toRoot, driveRoot);
        char letter = '\0';
        foreach (var c in "ZYXWVUTSRQPONMLKJIHGFED")
            if (!Directory.Exists(c + @":\") && !DriveInfo.GetDrives().Any(d => d.Name[0] == c)) { letter = c; break; }
        void Subst(string drive, string? target)
        {
            var p = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", target == null ? $"/c subst {drive}: /D" : $"/c subst {drive}: \"{target}\"")
                { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!;
            p.StandardOutput.ReadToEnd();
            p.WaitForExit();
        }
        var substMade = false;
        try
        {
            Check("a junction to a junction to Programs: the folder below it is refused", Refused(Path.Combine(chain1, "MCLA", "game")));
            Check("a junction to a drive root: the way to Programs below it is refused", Refused(Path.Combine(toRoot, programs[driveRoot.Length..], "MCLA", "game")),
                  Path.Combine(toRoot, programs[driveRoot.Length..], "MCLA", "game"));
            Check("a junction to a drive root: a folder that is not around the launcher is not refused for it", PathBudget.LauncherFolderProblem(Path.Combine(toRoot, "mcv", "p4", "other-game")) == null);
            if (letter == '\0') Check("a free drive letter for subst", false);
            else
            {
                Subst(letter.ToString(), programs);
                substMade = Directory.Exists(letter + @":\");
                Check($"subst {letter}: made", substMade);
                Check("a substituted drive that stands for Programs: MCLA below it is refused", Refused(letter + @":\MCLA\game"));
                Check("and a folder on it that is not around the launcher's is not refused for it", PathBudget.LauncherFolderProblem(letter + @":\Other\game") == null);
                Subst(letter.ToString(), null);
                substMade = false;
                // The launcher started from a substituted drive, the install folder typed with the real letters (and the other way round).
                Subst(letter.ToString(), parent);
                substMade = Directory.Exists(letter + @":\");
                var launcherThroughSubst = letter + @":\" + Path.GetFileName(here);
                Check("a launcher started through a substituted drive: the real path of its folder is refused", PathBudget.LauncherFolderProblem(Path.Combine(here, "game"), launcherThroughSubst) != null);
                Check("and so is the path through the substituted drive, with the launcher's real path", PathBudget.LauncherFolderProblem(Path.Combine(launcherThroughSubst, "game")) != null);
                Subst(letter.ToString(), null);
                substMade = false;
            }
        }
        finally
        {
            if (substMade) Subst(letter.ToString(), null);
            foreach (var link in new[] { chain1, chain2, toRoot })
                if (Directory.Exists(link)) Directory.Delete(link);
        }
        Check("the substituted drive is gone", letter == '\0' || !Directory.Exists(letter + @":\"));

        // R2: a launcher in a drive root is asked to move (not every folder refused); the example is not the launcher's own folder;
        // a folder that holds a launcher (installed.txt or MclaLauncher.exe) is not an install folder either.
        var rootText = PathBudget.LauncherFolderProblem(Path.Combine(scratch, "fine"), driveRoot);
        Check("a launcher in a drive root is asked to move", rootText != null && rootText.Contains("root of drive"), rootText ?? "accepted");
        var mcla = PathBudget.LauncherFolderProblem(@"C:\MCLA\game", @"C:\MCLA");
        Check("the example is not the launcher's own folder", mcla != null && mcla.Contains(@"for example C:\MCLA-game") && !mcla.Contains(@"for example C:\MCLA."), mcla ?? "accepted");
        var other = Path.Combine(scratch, "other");
        Directory.CreateDirectory(Path.Combine(other, "game"));
        File.WriteAllText(Path.Combine(other, "MclaLauncher.exe"), "x");
        Check("below a folder that holds a launcher is refused", PathBudget.LauncherFolderProblem(Path.Combine(other, "game", "deeper")) is { } t1 && t1.Contains("holds a launcher"));
        Check("a folder beside it is not", PathBudget.LauncherFolderProblem(other + "-game") == null);
        File.Delete(Path.Combine(other, "MclaLauncher.exe"));
        File.WriteAllText(Path.Combine(other, "installed.txt"), "x");
        Check("an installed.txt without Setup's keys (another program's) does not make a launcher's folder", PathBudget.LauncherFolderProblem(Path.Combine(other, "game")) == null);
        File.WriteAllText(Path.Combine(other, "installed.txt"), "version=2\ntag=draft-x\ncommit=abc\n");
        Check("installed.txt with version= and tag= marks a launcher's folder", PathBudget.LauncherFolderProblem(Path.Combine(other, "game")) is { } t2 && t2.Contains("holds a launcher"));
        File.WriteAllText(Path.Combine(other, "installed.txt"), "version=2\n");
        Check("... and with only one of the two it does not", PathBudget.LauncherFolderProblem(Path.Combine(other, "game")) == null);

        // A launcher in a drive root asks to move, for folders of its own drive only; the other drives are not in its way.
        var otherLetter = driveRoot[0] == 'Q' ? 'R' : 'Q';
        Check("a launcher in a drive root leaves the other drives alone", PathBudget.LauncherFolderProblem(otherLetter + @":\mcla-game", driveRoot) == null);
        Check("... and a folder of its own drive is asked to be elsewhere", PathBudget.LauncherFolderProblem(Path.Combine(driveRoot, "mcla-game"), driveRoot) is { } t3 && t3.Contains("root of drive"));

        // The launcher's own folders are worked out once: the wizard asks on every keystroke.
        var timer = System.Diagnostics.Stopwatch.StartNew();
        for (var i = 0; i < 200; i++) PathBudget.LauncherFolderProblem(Path.Combine(scratch, "fine" + i));
        Check($"200 verdicts take {timer.ElapsedMilliseconds} ms", timer.ElapsedMilliseconds < 3000);
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
            File.WriteAllText(Path.Combine(tools, f, ".mcla-checked"), "sha\n");
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
        // V1: the folder the record names must be there, with its marker, for another folder of the tool to be superseded.
        File.WriteAllText(record, "llvm-mingw\t" + folders[0] + "\tsha\ncmake\t" + folders[1] + "\tsha\nninja\tninja-9.9.9\tsha\nmingit\t" + folders[4] + "\tsha\n");
        Check("a record that names a folder that is not there supersedes nothing of that tool", ToolInstaller.Superseded(tools).Count == 0, Names(ToolInstaller.Superseded(tools)));
        File.WriteAllText(record, "llvm-mingw\t" + folders[0] + "\tsha\ncmake\t" + folders[1] + "\tsha\nninja\t" + folders[3] + "\tshaOther\nmingit\t" + folders[4] + "\tsha\n");
        Mark(folders[3], "sha");
        Check("a record whose folder holds another archive's marker supersedes nothing of that tool", ToolInstaller.Superseded(tools).Count == 0, Names(ToolInstaller.Superseded(tools)));
        File.WriteAllText(record, "llvm-mingw\t" + folders[0] + "\tsha\ncmake\t" + folders[1] + "\tsha\nninja\t" + folders[3] + "\tsha\nmingit\t" + folders[4] + "\tsha\n");
        Directory.Delete(Path.Combine(tools, folders[3]), true);
        Check("the same when the recorded folder is gone, though another ninja folder is there", ToolInstaller.Superseded(tools).Count == 0, Names(ToolInstaller.Superseded(tools)));
        Directory.CreateDirectory(Path.Combine(tools, folders[3]));
        Mark(folders[3], "sha");
        // V2: a folder set aside by an activation that was cut short is the only copy while the recorded folder is missing.
        Directory.Move(Path.Combine(tools, folders[3]), Path.Combine(tools, ".u-old-" + folders[3]));
        Check("a set-aside folder that is the only copy of the recorded one is not listed", ToolInstaller.Superseded(tools).Count == 0 || Names(ToolInstaller.Superseded(tools)) == "ninja-1.13.2", Names(ToolInstaller.Superseded(tools)));
        ToolInstaller.RestoreAside(tools);
        Check("and goes back to its place", Directory.Exists(Path.Combine(tools, folders[3])) && !Directory.Exists(Path.Combine(tools, ".u-old-" + folders[3])));
        Mark(folders[3], "shaC");
        // A temporary folder of an unpacking that was cut short goes whatever the record says; the unmarked folder never.
        Directory.CreateDirectory(Path.Combine(tools, ".u-ninja"));
        File.Delete(record);
        Check("a temporary folder is listed even without a record, the unmarked one never", Names(ToolInstaller.Superseded(tools)) == ".u-ninja", Names(ToolInstaller.Superseded(tools)));
        // With no record, a folder set aside (.u-old-*) may be the only copy of a tool: it is not listed; other .u-* names are not Setup's temporary ones.
        Directory.CreateDirectory(Path.Combine(tools, ".u-old-ninja-1.13.3"));
        Directory.CreateDirectory(Path.Combine(tools, ".u-something"));
        Check("with no record only the temporary folders of an unpacking are listed", Names(ToolInstaller.Superseded(tools)) == ".u-ninja", Names(ToolInstaller.Superseded(tools)));
        File.WriteAllText(record, "");
        Check("... also when the record is empty (no whole line in it)", Names(ToolInstaller.Superseded(tools)) == ".u-ninja", Names(ToolInstaller.Superseded(tools)));
        File.Delete(record);
        SafeFolder.Delete(Path.Combine(tools, ".u-old-ninja-1.13.3"));
        SafeFolder.Delete(Path.Combine(tools, ".u-something"));
        SafeFolder.Delete(Path.Combine(tools, ".u-ninja"));

        // The record is replaced whole (a leftover temporary file does not matter) and keeps the other tools.
        File.WriteAllText(record + ".tmp", "leftover from a stop");
        void Mark(string folder, string sha) => File.WriteAllText(Path.Combine(tools, folder, ".mcla-checked"), sha + "\n");
        Mark(folders[2], "shaA"); Mark(folders[3], "shaC"); Mark(folders[4], "shaB");
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

        // V2: if the staged folder can't be moved in, the folder that was in use goes back (it may be the only copy).
        var t2 = Path.Combine(scratch, "tools2");
        Directory.CreateDirectory(Path.Combine(t2, "ninja-5"));
        File.WriteAllText(Path.Combine(t2, "ninja-5", ".mcla-checked"), "old\n");
        File.WriteAllText(Path.Combine(t2, "ninja-5", "ninja.exe"), "old");
        File.WriteAllText(Path.Combine(t2, ".mcla-tools.txt"), "ninja\tninja-5\told\n");
        Directory.CreateDirectory(Path.Combine(t2, ".staged", "ninja-5"));
        File.WriteAllText(Path.Combine(t2, ".staged", "ninja-5", ".mcla-checked"), "new\n");
        File.WriteAllText(Path.Combine(t2, ".staged", "ninja-5", "ninja.exe"), "new");
        using (var held = new FileStream(Path.Combine(t2, ".staged", "ninja-5", "ninja.exe"), FileMode.Open, FileAccess.Read, FileShare.None))
        {
            var failed = false;
            try { ToolInstaller.Activate(t2, "ninja", "ninja-5", "new"); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { failed = true; }
            Check("activation fails while the staged folder is held open", failed);
        }
        Check("the folder that was in use is back and the record is as it was", File.ReadAllText(Path.Combine(t2, "ninja-5", "ninja.exe")) == "old"
              && !Directory.Exists(Path.Combine(t2, ".u-old-ninja-5")) && File.ReadAllText(Path.Combine(t2, ".mcla-tools.txt")).StartsWith("ninja\tninja-5\told"));
        ToolInstaller.Activate(t2, "ninja", "ninja-5", "new");
        Check("and the next try works", File.ReadAllText(Path.Combine(t2, "ninja-5", "ninja.exe")) == "new" && File.ReadAllText(Path.Combine(t2, ".mcla-tools.txt")).StartsWith("ninja\tninja-5\tnew"));

        // V3: what install.json names is a tool and plain names.
        var l3 = new InstallLayout(Path.Combine(scratch, "install3"));
        Directory.CreateDirectory(l3.Tools);
        var outside = Path.Combine(scratch, "outside-tools");
        Directory.CreateDirectory(outside);
        File.WriteAllText(Path.Combine(outside, ".mcla-checked"), "x\n");
        var logged = new List<string>();
        UpdateEngine.ActivateTools(l3, [new PendingTool("ninja", @"..\..\outside-tools", "x", "a.zip"), new PendingTool("notatool", "f", "x", "a.zip"), new PendingTool("ninja", "ok-1", "x", @"..\a.zip")],
                                   logged.Add, CancellationToken.None);
        Check("a record that names a folder outside tools\\, an unknown tool, or an archive with a path is skipped",
              logged.Count(l => l.Contains("skipped")) == 3 && File.Exists(Path.Combine(outside, ".mcla-checked")) && !File.Exists(Path.Combine(l3.Tools, ".mcla-tools.txt")), string.Join(" | ", logged));

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

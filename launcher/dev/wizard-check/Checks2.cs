// More checks of the setup's safety nets (the review's fix round): git found in Installed mode, one setup at a
// time in a folder, a child that ends with its parent, retries of renames, what an archive may hold, what is
// left to do after a partial run, what a release.json may name, and what an uninstall refuses.
using System.Diagnostics;
using System.Text.Json;
using MclaLauncher.Setup;

namespace MclaLauncher;

static partial class Program
{
    /// Run from a copy of this program with an install.json beside it (so that it is in Installed mode) and with a
    /// PATH that has no git: the launcher's own status read (Repo.GetStatusAsync, what the window does at start).
    static async Task GitInInstalledMode()
    {
        Console.WriteLine($"mode {Paths.Mode}; install {Paths.InstallRoot}; GitDir {Paths.GitDir}; GitExe {Paths.GitExe}");
        Check("this run is in Installed mode", Paths.Mode == LaunchMode.Installed);
        var onPath = Paths.FindGit(null, Environment.GetEnvironmentVariable("PATH"));
        Console.WriteLine($"git on this process's PATH: {(onPath == "git" ? "none" : onPath)}");
        Check("git is MinGit's from the install", Paths.GitExe.StartsWith(Paths.GitDir ?? "?", StringComparison.OrdinalIgnoreCase), Paths.GitExe);
        var version = await Proc.RunAsync("git", ["--version"], TimeSpan.FromSeconds(20));
        Check("git --version runs through Proc.RunAsync", version.ExitCode == 0, version.Output);
        var status = await Repo.GetStatusAsync(false);
        Check("the repository's status is read (what the launcher does at start)", !string.IsNullOrEmpty(status.Head), $"HEAD {status.Head}; game built {status.GameBuilt}; build current {status.BuildCurrent}");
    }

    static void LockChecks(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var root = Path.Combine(scratch, "install");
        using (var first = InstallLock.Acquire(root))
        {
            Check("the lock is held", InstallLock.IsHeld(root));
            try { InstallLock.Acquire(root).Dispose(); Check("a second setup in the folder is refused", false); }
            catch (SetupException ex) { Check("a second setup in the folder is refused", true, ex.Plain); }
            new InstallRecord { InstallRoot = root, State = "setting-up" }.Save(Path.Combine(root, InstallLayout.RecordName));
            var problem = PathBudget.Problem(root, out var resumes);
            Check("the folder page says a setup is running there", problem != null && problem.Contains("running") && !resumes, problem ?? "no problem");
            try { InstallCleanup.Uninstall(root, null); Check("an uninstall while a setup runs is refused", false); }
            catch (SetupException ex) { Check("an uninstall while a setup runs is refused", true, ex.Plain); }
            Check("and nothing was removed", File.Exists(Path.Combine(root, InstallLayout.RecordName)));
        }
        Check("the lock is let go", !InstallLock.IsHeld(root) && !File.Exists(Path.Combine(root, InstallLock.FileName)));
        Check("then the folder resumes", PathBudget.Problem(root, out var again) == null && again);
    }

    /// Starts a program in the job, writes its id, then ends this process the hard way: the program must go too.
    static void Orphan(string pidFile, bool withJob)
    {
        ChildJob.Enabled = withJob;
        var psi = new ProcessStartInfo("ping.exe", "-n 300 127.0.0.1") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
        var child = Process.Start(psi)!;
        ChildJob.AssignIfEnabled(child);
        File.WriteAllText(pidFile, child.Id.ToString());
        Console.WriteLine($"started ping {child.Id}; ending this process");
        Process.GetCurrentProcess().Kill();
    }

    static void RetryChecks(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var from = Path.Combine(scratch, "fresh.bin");
        var to = Path.Combine(scratch, "placed.bin");
        File.WriteAllText(from, "x");
        // A program (a scanner) holds the file for two seconds.
        var holder = new FileStream(from, FileMode.Open, FileAccess.Read, FileShare.None);
        var release = Task.Run(async () => { await Task.Delay(2000); holder.Dispose(); });
        var clock = Stopwatch.StartNew();
        try { File.Move(from, to); Check("a rename of a held file fails without a retry", false); }
        catch (IOException) { Check("a rename of a held file fails without a retry", true); }
        Retry.Io(() => File.Move(from, to));
        release.Wait();
        Check("the retry waits it out", File.Exists(to) && !File.Exists(from), $"{clock.Elapsed.TotalSeconds:0.0} s");
        // A folder that stays held is given up after about ten seconds, not for ever.
        var stuck = Path.Combine(scratch, "stuck.bin");
        File.WriteAllText(stuck, "x");
        using var held = new FileStream(stuck, FileMode.Open, FileAccess.Read, FileShare.None);
        clock.Restart();
        try { Retry.Io(() => File.Move(stuck, stuck + ".moved"), TimeSpan.FromSeconds(2)); Check("a file held for good is given up", false); }
        catch (IOException) { Check("a file held for good is given up", clock.Elapsed.TotalSeconds is > 1.5 and < 6, $"{clock.Elapsed.TotalSeconds:0.0} s"); }
        // A missing file is not waited for.
        clock.Restart();
        try { Retry.Io(() => File.Move(Path.Combine(scratch, "nothing"), Path.Combine(scratch, "x"))); }
        catch (FileNotFoundException) { }
        Check("a missing file is not waited for", clock.Elapsed.TotalSeconds < 1);
    }

    /// What an archive may hold, and the record of a tool that is unpacked.
    static async Task ArchiveChecks(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        foreach (var odd in new[] { "file:stream", "dir/file.txt:ads", "CON", "nul.txt", "sub/COM1.log", "LPT9", "trailing.", "dir./file", "space ", "a|b", "star*", "q?" })
            Check($"odd name refused: {odd}", ToolInstaller.IsOddName(odd));
        foreach (var fine in new[] { "bin/clang.exe", "share/doc/readme.txt", "lib/libc++.a", "x.y.z/..data", "console.txt", "com10.txt", "a b/c d.txt", "include/c++/v1/__config" })
            Check($"normal name accepted: {fine}", !ToolInstaller.IsOddName(fine));

        var log = new List<string>();
        var zip = Path.Combine(scratch, "ads.zip");
        using (var z = System.IO.Compression.ZipFile.Open(zip, System.IO.Compression.ZipArchiveMode.Create))
        {
            using (var w = new StreamWriter(z.CreateEntry("tool-1/bin/ok.exe").Open())) w.Write("x");
            using (var w = new StreamWriter(z.CreateEntry("tool-1/bin/ok.exe:hidden").Open())) w.Write("y");
        }
        try
        {
            await ToolInstaller.InstallAsync(Path.Combine(scratch, "tools"), "ads", "1", zip, "11", log.Add, new Progress<(int, int)>(), CancellationToken.None);
            Check("an archive with an alternate data stream is refused", false);
        }
        catch (SetupException ex) { Check("an archive with an alternate data stream is refused", true, ex.Plain); }
        Check("and nothing is left of it", !Directory.Exists(Path.Combine(scratch, "tools", "tool-1")));

        var good = Path.Combine(scratch, "good.zip");
        using (var z = System.IO.Compression.ZipFile.Open(good, System.IO.Compression.ZipArchiveMode.Create))
            using (var w = new StreamWriter(z.CreateEntry("tool-2/bin/tool.exe").Open())) w.Write("x");
        var tools = Path.Combine(scratch, "tools");
        var sha = new string('a', 64);
        Check("a tool that was never unpacked is not 'installed'", !ToolInstaller.IsInstalled(tools, "tool", sha));
        var folder = await ToolInstaller.InstallAsync(tools, "tool", "2", good, sha, log.Add, new Progress<(int, int)>(), CancellationToken.None);
        Check("an unpacked tool is 'installed' for its archive's hash", ToolInstaller.IsInstalled(tools, "tool", sha));
        Check("but not for another archive", !ToolInstaller.IsInstalled(tools, "tool", new string('b', 64)));
        Check("nor under another name", !ToolInstaller.IsInstalled(tools, "other", sha));
        File.Delete(Path.Combine(folder, ".mcla-checked"));
        Check("nor when its marker is gone", !ToolInstaller.IsInstalled(tools, "tool", sha));
    }

    /// release.json that names things that can't be used.
    static void ReleaseNameChecks(string folder)
    {
        var good = File.ReadAllText(Path.Combine(folder, "release.json"));
        var release = ReleaseInfo.Parse(good);
        void Refuse(string what, Func<string, string> alter)
        {
            try { ReleaseInfo.Parse(alter(good)); Check($"refused: {what}", false, "accepted"); }
            catch (SetupException ex) { Check($"refused: {what}", true, ex.Plain); }
        }
        var bundle = release.SdkBundle!.Asset;
        Refuse("an SDK bundle name with a folder in it", j => j.Replace($"\"asset\": \"{bundle}\"", "\"asset\": \"..\\\\..\\\\x.zip\""));
        Refuse("an SDK bundle name with a drive", j => j.Replace($"\"asset\": \"{bundle}\"", "\"asset\": \"C:x.zip\""));
        Refuse("an SDK bundle name that is a device", j => j.Replace($"\"asset\": \"{bundle}\"", "\"asset\": \"NUL\""));
        var cmakeUrl = release.Tools["cmake"].Url;
        Refuse("a tool at an http address", j => j.Replace(cmakeUrl, cmakeUrl.Replace("https://", "http://")));
        Refuse("a tool at a network share", j => j.Replace(cmakeUrl, @"\\\\server\\share\\cmake.zip"));
        Refuse("a tool whose file name has a stream", j => j.Replace(cmakeUrl, cmakeUrl + ":x"));
        Refuse("a shader pack outside the checkout", j => j.Replace(release.ShaderPack!.Path, "../../x.shaderpack"));
        Refuse("a tool version with a separator", j => j.Replace($"\"version\": \"{release.Tools["ninja"].Version}\"", "\"version\": \"1/../2\""));
        Check("the real one is accepted", true);
    }

    static async Task NonHttpsChecks(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var local = Path.Combine(scratch, "a.bin");
        File.WriteAllText(local, "x");
        try { await ReleaseSource.DownloadAsync(local, Path.Combine(scratch, "b.bin"), new Progress<long>(), CancellationToken.None); Check("a local file is refused outside the dev override", false); }
        catch (SetupException ex) { Check("a local file is refused outside the dev override", true, ex.Plain); }
        try { await ReleaseSource.DownloadAsync(@"\\server\share\a.bin", Path.Combine(scratch, "b.bin"), new Progress<long>(), CancellationToken.None); Check("a network share is refused", false); }
        catch (SetupException ex) { Check("a network share is refused", true, ex.Plain); }
        await ReleaseSource.DownloadAsync(local, Path.Combine(scratch, "c.bin"), new Progress<long>(), CancellationToken.None, allowLocalFile: true);
        Check("the dev override's local file is copied", File.Exists(Path.Combine(scratch, "c.bin")));
    }

    /// What is still to do (the disk preflight) after part of the setup.
    static void RemainingChecks(string scratch)
    {
        SafeFolder.Delete(scratch);
        var layout = new InstallLayout(Path.Combine(scratch, "i"));
        Directory.CreateDirectory(layout.Root);
        const long Gb = 1L << 30;
        long game = 6 * Gb;
        var fresh = new InstallRecord { InstallRoot = layout.Root };
        Check("a fresh folder needs the game and 3 GB", PathBudget.RemainingBytes(layout, fresh, game, true, false, false) == PathBudget.RequiredBytes(game, true), PathBudget.RemainingBytes(layout, fresh, game, true, false, false).ToString());
        Check("a game kept where it is needs 3 GB", PathBudget.RemainingBytes(layout, fresh, game, false, false, false) == 3 * Gb);
        Directory.CreateDirectory(layout.Staging);
        // A smaller "game", so that the files made here are small.
        long smallGame = 100L << 20;
        using (var f = new FileStream(Path.Combine(layout.Staging, "part.bin"), FileMode.Create)) f.SetLength(60L << 20);
        var half = PathBudget.RemainingBytes(layout, fresh, smallGame, true, false, false);
        Check("a game copied by 60% needs only the other 40% and the 3 GB", half == (40L << 20) + 3 * Gb, half.ToString());
        var done = fresh with { StepsDone = ["Game"] };
        Check("a finished game needs only the 3 GB", PathBudget.RemainingBytes(layout, done, smallGame, true, false, false) == 3 * Gb);
        Check("with the tools unpacked it needs 1.5 GB", PathBudget.RemainingBytes(layout, done, smallGame, true, true, false) == 3 * Gb / 2);
        Check("with the game built it needs nothing", PathBudget.RemainingBytes(layout, done, smallGame, true, true, true) == 0);
    }

    /// What an uninstall refuses, and the removal of the names the record lists only.
    static void CleanupChecks(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        foreach (var bad in new[] { @"C:\", @"C:\Users", profile, Path.Combine(profile, "Downloads"), Path.Combine(profile, "AppData", "Local"), Environment.GetFolderPath(Environment.SpecialFolder.Windows), Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles) })
            Check($"never an install: {bad}", InstallCleanup.Refusal(bad) != null);
        Check("a folder of its own is fine", InstallCleanup.Refusal(Path.Combine(scratch, "install")) == null, InstallCleanup.Refusal(Path.Combine(scratch, "install")) ?? "");
        Check("so is a folder under the profile", InstallCleanup.Refusal(Path.Combine(profile, "Documents", "MCLA")) == null);

        // A forged record that lists a name outside the folder.
        var root = Path.Combine(scratch, "forged");
        Directory.CreateDirectory(Path.Combine(root, "mine"));
        File.WriteAllText(Path.Combine(root, "mine", "x.txt"), "keep");
        var outside = Path.Combine(scratch, "outside");
        Directory.CreateDirectory(outside);
        File.WriteAllText(Path.Combine(outside, "precious.txt"), "keep");
        new InstallRecord { InstallRoot = root, Owned = ["..\\outside", "tools"] }.Save(Path.Combine(root, InstallLayout.RecordName));
        try { InstallCleanup.Uninstall(root, null); Check("a name with a folder in it is refused", false); }
        catch (SetupException ex) { Check("a name with a folder in it is refused", true, ex.Plain); }
        Check("and nothing outside was touched", File.Exists(Path.Combine(outside, "precious.txt")));

        // Only the listed names go.
        var root2 = Path.Combine(scratch, "listed");
        Directory.CreateDirectory(Path.Combine(root2, "tools"));
        Directory.CreateDirectory(Path.Combine(root2, "Documents"));
        File.WriteAllText(Path.Combine(root2, "Documents", "a.txt"), "the user's");
        new InstallRecord { InstallRoot = root2 }.Save(Path.Combine(root2, InstallLayout.RecordName));
        var result = InstallCleanup.Uninstall(root2, null);
        Check("what the record lists is removed, and only that", !Directory.Exists(Path.Combine(root2, "tools")) && File.Exists(Path.Combine(root2, "Documents", "a.txt")), result.Summary);

        // A game kept inside what would be removed.
        var root3 = Path.Combine(scratch, "inside");
        var game = Path.Combine(root3, "tools", "game");
        Directory.CreateDirectory(game);
        new InstallRecord { InstallRoot = root3, GameMode = "in-place", GameDataRoot = game }.Save(Path.Combine(root3, InstallLayout.RecordName));
        try { InstallCleanup.Uninstall(root3, null); Check("a game kept inside a folder to remove is refused", false); }
        catch (SetupException ex) { Check("a game kept inside a folder to remove is refused", true, ex.Plain); }
        Check("and it is still there", Directory.Exists(game));
    }
}

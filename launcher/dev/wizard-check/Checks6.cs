// Checks of where the launcher decides it is (Paths.Locate, fork issue 81): started from a folder directly under a drive's root
// (D:\Games\launcher.exe by a third-party launcher), the setup wizard's "folder above the exe" was the root, which has no parent,
// and the type initializer of Paths threw ArgumentNullException (path1) before any window opened. Nothing here writes outside
// <scratch>: the exe folders under the real drive root are never created, only looked at (they hold no build-game.ps1).
//   paths <scratch>    exe under a drive root's folder, at the root, deeper, a normal checkout, an install with install.json,
//                      a UNC share, a path that makes the detection throw, no base folder at all
using MclaLauncher.Setup;

namespace MclaLauncher;

static partial class Program
{
    static void PathsChecks(string scratch)
    {
        scratch = Path.GetFullPath(scratch);
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var drive = Path.GetPathRoot(Path.GetFullPath(scratch))!;                    // C:\ (the one the checks run on)
        if (File.Exists(Path.Combine(drive, "build-game.ps1")) || File.Exists(Path.Combine(drive, InstallLayout.RecordName)))
        {
            Check("the drive's root holds no build-game.ps1 or install.json (the checks assume so)", false, drive);
            return;
        }

        /// What the type initializer makes of the places: every one a real path, Combine and the folder above both work on them.
        void Usable(string what, Paths.Places p)
        {
            var all = new[] { p.LauncherDir, p.Repo };
            Check($"{what}: no null or empty folder", all.All(s => !string.IsNullOrEmpty(s)));
            try
            {
                var derived = new[] { Path.Combine(p.Repo, "out"), Path.Combine(p.Repo, "out", @"build\win-amd64-release", "mcla.exe"), Path.Combine(p.LauncherDir, "build-game.ps1"),
                                      Path.Combine(p.LauncherDir, "settings.json"), Path.Combine(p.LauncherDir, "logs") };
                Check($"{what}: the paths Paths derives can be made", derived.All(s => s.Length > 0));
            }
            catch (Exception ex) { Check($"{what}: the paths Paths derives can be made", false, ex.Message); }
        }

        // Directly under a drive's root: the issue. The exe folder is as AppContext.BaseDirectory gives it, with a trailing backslash.
        var under = drive + "MclaRootCheckFolder";
        var p1 = Paths.Locate(under + "\\");
        Console.WriteLine($"      {under}\\: {p1.Mode}, launcher {p1.LauncherDir}, repo {p1.Repo}");
        Usable("a folder directly under a drive root", p1);
        Check("it is the setup wizard, with no install", p1.Mode == LaunchMode.Wizard && p1.InstallRoot == null && p1.Problem == null);
        Check("the launcher folder is the exe's own folder, not the root", string.Equals(p1.LauncherDir, under, StringComparison.OrdinalIgnoreCase), p1.LauncherDir);
        Check("the repo is not the root, nor null", string.Equals(p1.Repo, under, StringComparison.OrdinalIgnoreCase), p1.Repo);
        var p1b = Paths.Locate(under);
        Check("without the trailing backslash it is the same", p1b == p1);

        // In the root itself: there is no folder to use but the root; nothing in the wizard writes there.
        var p2 = Paths.Locate(drive);
        Console.WriteLine($"      {drive}: {p2.Mode}, launcher {p2.LauncherDir}, repo {p2.Repo}");
        Usable("the exe in a drive's root", p2);
        Check("it is the setup wizard, with no problem to log", p2.Mode == LaunchMode.Wizard && p2.InstallRoot == null && p2.Problem == null);
        Check("the launcher folder and the repo are the root itself", string.Equals(p2.LauncherDir, drive, StringComparison.OrdinalIgnoreCase) && p2.Repo == p2.LauncherDir);

        // Two levels down: the folder above the exe is not a root, but the one above that is: the repo stays off the root.
        var p3 = Paths.Locate(drive + @"MclaRootCheckFolder\app");
        Usable("an exe two folders below the root", p3);
        Check("the launcher folder is the folder above the exe", string.Equals(p3.LauncherDir, under, StringComparison.OrdinalIgnoreCase), p3.LauncherDir);
        Check("the repo is not the root either", string.Equals(p3.Repo, under, StringComparison.OrdinalIgnoreCase), p3.Repo);

        // Deeper: what it has always been (the exe's folder's parent is the launcher folder, and that one's parent the repo).
        var p4 = Paths.Locate(drive + @"MclaRootCheckFolder\a\b\c");
        Usable("an exe deeper down", p4);
        Check("the launcher folder is the folder above the exe, the repo the one above that",
              string.Equals(p4.LauncherDir, drive + @"MclaRootCheckFolder\a\b", StringComparison.OrdinalIgnoreCase) && string.Equals(p4.Repo, drive + @"MclaRootCheckFolder\a", StringComparison.OrdinalIgnoreCase),
              $"{p4.LauncherDir}, {p4.Repo}");

        // A normal checkout: launcher\build-game.ps1 above the exe. Classic; the repo is the checkout.
        var checkout = Path.Combine(scratch, "classic", "mcla-recomp");
        Directory.CreateDirectory(Path.Combine(checkout, "launcher", "app"));
        File.WriteAllText(Path.Combine(checkout, "launcher", "build-game.ps1"), "");
        var p5 = Paths.Locate(Path.Combine(checkout, "launcher", "app") + "\\");
        Usable("a checkout's launcher\\app", p5);
        Check("it is Classic; the launcher folder is launcher\\, the repo the checkout",
              p5.Mode == LaunchMode.Classic && p5.InstallRoot == null && p5.Problem == null
              && string.Equals(p5.LauncherDir, Path.Combine(checkout, "launcher"), StringComparison.OrdinalIgnoreCase) && string.Equals(p5.Repo, checkout, StringComparison.OrdinalIgnoreCase),
              $"{p5.Mode}, {p5.LauncherDir}, {p5.Repo}");
        // A checkout that is a drive's root (the repo is the root, with launcher\ in it): that is a repo, whatever the drive.
        Check("a launcher\\ directly in a drive's root has the root as its repo", Paths.RepoOf(LaunchMode.Classic, null, drive + "launcher") == drive);
        Check("a Classic launcher folder that is the root has itself as the repo, no null", Paths.RepoOf(LaunchMode.Classic, null, drive) == drive);

        // An install with install.json beside the exe: finished (Installed) and not finished (the wizard resumes it).
        var root = Path.Combine(scratch, "install");
        var exe = Path.Combine(scratch, "app");
        Directory.CreateDirectory(exe);
        var layout = new InstallLayout(root);
        new InstallRecord { InstallRoot = root, State = "setting-up" }.Save(InstallRecord.PointerFile(exe));
        var p6 = Paths.Locate(exe + "\\");
        Usable("an unfinished install", p6);
        Check("the wizard resumes it: its install root, the checkout's folders", p6.Mode == LaunchMode.Wizard && p6.Problem == null
              && string.Equals(p6.InstallRoot, root, StringComparison.OrdinalIgnoreCase) && string.Equals(p6.LauncherDir, layout.LauncherDir, StringComparison.OrdinalIgnoreCase)
              && string.Equals(p6.Repo, layout.Repo, StringComparison.OrdinalIgnoreCase), $"{p6.Mode}, {p6.InstallRoot}, {p6.LauncherDir}, {p6.Repo}");
        Directory.CreateDirectory(layout.LauncherDir);
        File.WriteAllText(layout.BuildScript, "");
        new InstallRecord { InstallRoot = root, State = "ready" }.Save(InstallRecord.PointerFile(exe));
        var p7 = Paths.Locate(exe + "\\");
        Usable("a finished install", p7);
        Check("it is Installed: the install root, the checkout's folders", p7.Mode == LaunchMode.Installed && p7.Problem == null
              && string.Equals(p7.InstallRoot, root, StringComparison.OrdinalIgnoreCase) && string.Equals(p7.LauncherDir, layout.LauncherDir, StringComparison.OrdinalIgnoreCase)
              && string.Equals(p7.Repo, layout.Repo, StringComparison.OrdinalIgnoreCase), $"{p7.Mode}, {p7.InstallRoot}, {p7.LauncherDir}, {p7.Repo}");
        // An install in the root of a drive: its checkout is in a folder of its own, mcla-recomp\.
        var rootInstall = new InstallLayout(drive.TrimEnd('\\') + "\\");
        Check("an install folder that is a drive's root still has a repo below it", Paths.RepoOf(LaunchMode.Wizard, rootInstall.Root, rootInstall.LauncherDir) == rootInstall.Repo);

        // A folder on a share (the pure parts: nothing here reaches the network).
        const string share = @"\\server\share";
        Check("an exe folder directly on a share: its own folder", Paths.WizardFolder(share + @"\X") == share + @"\X");
        Check("an exe in the share's root: its own folder, no null", Paths.WizardFolder(share + "\\") == share);
        Check("the repo of the wizard directly on a share is the launcher folder", Paths.RepoOf(LaunchMode.Wizard, null, share + @"\X") == share + @"\X");
        Check("the folder above the exe's, when it is not the root", Paths.WizardFolder(share + @"\X\Y") == share + @"\X");

        // Anything that goes wrong becomes the wizard in the exe's own folder, with the reason to log: a character Windows refuses.
        var bad = Paths.Locate(drive + "MclaRootCheck\0Folder");
        Console.WriteLine($"      refused path: {bad.Mode}, launcher {bad.LauncherDir.Replace("\0", "<NUL>")}; {bad.Problem}");
        Check("a path the detection throws on gives the wizard in the exe's folder", bad.Mode == LaunchMode.Wizard && bad.InstallRoot == null
              && !string.IsNullOrEmpty(bad.LauncherDir) && bad.Repo == bad.LauncherDir && bad.LauncherDir.StartsWith(drive, StringComparison.OrdinalIgnoreCase));
        Check("and says why", bad.Problem != null && bad.Problem.Contains("ArgumentException") && bad.Problem.Contains("starting the setup"), bad.Problem ?? "(none)");
        Usable("the refused path", bad);
        var logs = Path.Combine(scratch, "logs");
        Paths.LogStartProblem(logs, bad.Problem!);
        var logged = File.Exists(Path.Combine(logs, "launcher-start.log")) ? File.ReadAllText(Path.Combine(logs, "launcher-start.log")) : "";
        Check("the reason is written to launcher-start.log", logged.Contains("Could not work out where the launcher is installed"));
        Paths.LogStartProblem(Path.Combine(scratch, "app", "install.json", "nope"), "a folder that can't be made is no failure");   // a file is in the way
        Check("a log that can't be written is no failure", true);

        // No base folder at all: the current folder.
        foreach (var none in new string?[] { null, "", " " })
        {
            var p = Paths.Locate(none);
            Usable($"no base folder ({(none == null ? "null" : $"\"{none}\"")})", p);
        }
    }
}

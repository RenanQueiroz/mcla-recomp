// MCLA-Setup.exe: installs the launcher for the current user, updates it, or removes it.
//
//   MCLA-Setup.exe                       install (or repair) into %LOCALAPPDATA%\Programs\MCLA, make the
//                                        Start menu and desktop shortcuts, start the launcher
//   MCLA-Setup.exe --update --wait-pid N the same, started by the launcher itself: waits for process N (the
//                                        old launcher) to end, swaps the folders, starts the new launcher and,
//                                        if it does not come up, puts the old one back
//   MCLA-Setup.exe --uninstall           removes the launcher and its shortcuts (not the game install)
//
// No administrator rights are asked for or used. Options (all optional):
//   --dir <folder>        the launcher's folder (default %LOCALAPPDATA%\Programs\MCLA)
//   --silent              no windows; errors go to the exit code and the log (%TEMP%\MCLA-Setup.log)
//   --no-launch           do not start the launcher afterwards (and so no start check, no way back)
//   --no-shortcuts        no shortcuts
//   --force               replace a newer launcher with this one without asking
//   --keep <name>         a file of the old folder to carry over (repeatable; install.json and settings.json
//                         are always kept: the pointer from the launcher to its install)
//   --health window|alive:<seconds>   what "the new launcher started" means (default: a window, still
//                         open 3 s later; alive:N is for tests)
//   --launch-args <text>  arguments for the launcher (tests)
//   --rename-timeout <s>  how long to wait for Windows to let the old folder be renamed (default 60; tests)
//   --shortcut-dir <dir>  put the shortcuts under this folder instead of the Start menu and desktop (tests)
//
// For building and checking the release (a console program from a terminal):
//   --compose <folder> <out.exe> [--stub <exe>] [--quality 9] [--version V] [--tag T] [--commit C] [--exe MclaLauncher.exe]
//   --info [<exe>]        what the payload of an installer is
//   --extract <folder>    unpack the payload there, nothing else
//
// Exit codes: 0 done, 1 failed (nothing changed, or the old launcher was put back), 2 cancelled or declined.
using System.Diagnostics;
using System.Runtime.InteropServices;
using Mcla.Updating;

namespace Mcla.Setup;

sealed class SetupFailure(string message) : Exception(message);

sealed class Options
{
    public string Mode = "install"; // install | update | uninstall | compose | info | extract
    public string? Dir, ShortcutDir, Stub, ComposeFolder, ComposeOut, ExtractTo, InfoExe, LaunchArgs;
    public bool Silent, NoLaunch, NoShortcuts, Force;
    public int WaitPid, RenameSeconds = 60;
    public string Health = "window";
    public int Quality = 9;
    public PayloadMeta Meta = new();
    public List<string> Keep = ["install.json", "settings.json"];
}

static unsafe class Program
{
    [DllImport("kernel32.dll")]
    static extern bool AttachConsole(int pid);

    static StreamWriter? log;

    static int Main(string[] args)
    {
        Options o;
        try { o = ParseArguments(args); }
        catch (SetupFailure ex) { Console(); System.Console.Error.WriteLine(ex.Message); return 1; }

        if (o.Mode is "compose" or "info" or "extract") { Console(); return RunTool(o); }

        // One setup at a time for this user: a second double-click (or an update while an install runs) would
        // unpack into and clean up the same folders.
        using var single = new Mutex(true, @"Local\MCLA-Setup", out var first);
        if (!first)
        {
            if (!o.Silent) Dialogs.Info("Setup is already running. Wait for it to finish.");
            return 2;
        }

        try { log = new StreamWriter(Path.Combine(Path.GetTempPath(), "MCLA-Setup.log"), false) { AutoFlush = true }; }
        catch (Exception) { /* no log */ }
        Log($"MCLA-Setup {string.Join(' ', args)}  ({DateTime.Now:yyyy-MM-dd HH:mm:ss}, process {Environment.ProcessId})");

        // The Task Dialog needs the main thread's message loop; the work runs on another thread.
        var exit = 1;
        var work = new Thread(() =>
        {
            try { exit = Run(o); }
            catch (OperationCanceledException) { Log("cancelled"); exit = 2; }
            catch (SetupFailure ex) { Log("FAILED: " + ex.Message); Fail(o, ex.Message); exit = 1; }
            catch (Exception ex) { Log("FAILED: " + ex); Fail(o, "Something went wrong: " + ex.Message + "\n\nThe log is " + Path.Combine(Path.GetTempPath(), "MCLA-Setup.log")); exit = 1; }
            finally { if (!o.Silent) Dialogs.Close(); }
        });
        work.Start();
        if (!o.Silent)
        {
            Dialogs.ShowProgress(o.Mode == "uninstall" ? "Removing the launcher" : "Setting up the launcher", "Starting...");
        }
        work.Join();
        return exit;
    }

    static void Fail(Options o, string message) { if (!o.Silent) { Dialogs.Close(); Dialogs.Error(message); } }

    static void Console()
    {
        AttachConsole(-1); // the terminal this was started from, if any
    }

    static void Log(string line) { try { log?.WriteLine(line); } catch (Exception) { } }

    static string DefaultDir() => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "MCLA");

    // ---- arguments ---------------------------------------------------------------------------------------------

    static Options ParseArguments(string[] args)
    {
        var o = new Options();
        for (var i = 0; i < args.Length; i++)
        {
            string Next() => i + 1 < args.Length ? args[++i] : throw new SetupFailure($"{args[i]} needs a value");
            switch (args[i])
            {
                case "--install": o.Mode = "install"; break;
                case "--update": o.Mode = "update"; break;
                case "--uninstall": o.Mode = "uninstall"; break;
                case "--compose": o.Mode = "compose"; o.ComposeFolder = Next(); o.ComposeOut = Next(); break;
                case "--info": o.Mode = "info"; if (i + 1 < args.Length && !args[i + 1].StartsWith("--")) o.InfoExe = args[++i]; break;
                case "--extract": o.Mode = "extract"; o.ExtractTo = Next(); break;
                case "--dir": o.Dir = Next(); break;
                case "--silent": o.Silent = true; break;
                case "--no-launch": o.NoLaunch = true; break;
                case "--no-shortcuts": o.NoShortcuts = true; break;
                case "--force": o.Force = true; break;
                case "--keep": o.Keep.Add(Next()); break;
                case "--wait-pid": o.WaitPid = int.TryParse(Next(), out var pid) ? pid : throw new SetupFailure("--wait-pid needs a number"); break;
                case "--health": o.Health = Next(); break;
                case "--rename-timeout": o.RenameSeconds = int.TryParse(Next(), out var rt) && rt > 0 ? rt : throw new SetupFailure("--rename-timeout needs seconds"); break;
                case "--launch-args": o.LaunchArgs = Next(); break;
                case "--shortcut-dir": o.ShortcutDir = Next(); break;
                case "--stub": o.Stub = Next(); break;
                case "--quality": o.Quality = int.TryParse(Next(), out var q) && q is >= 0 and <= 11 ? q : throw new SetupFailure("--quality is 0 to 11"); break;
                case "--version": o.Meta.Values["version"] = Next(); break;
                case "--tag": o.Meta.Values["tag"] = Next(); break;
                case "--commit": o.Meta.Values["commit"] = Next(); break;
                case "--exe": o.Meta.Values["exe"] = Next(); break;
                case "-h": case "--help": case "/?": throw new SetupFailure("MCLA-Setup.exe [--update --wait-pid N | --uninstall] [--dir folder] [--silent] [--no-launch] [--no-shortcuts]");
                default: throw new SetupFailure($"unknown option {args[i]}");
            }
        }
        if (o.Mode == "update" && o.WaitPid == 0) throw new SetupFailure("--update needs --wait-pid <the old launcher's process id>");
        return o;
    }

    // ---- the build tools -----------------------------------------------------------------------------------------

    static int RunTool(Options o)
    {
        try
        {
            if (o.Mode == "compose")
            {
                var self = Environment.ProcessPath!;
                var stub = o.Stub ?? self;
                if (!File.Exists(stub)) throw new SetupFailure($"no installer program at {stub}");
                using (var own = Payload.Open(stub)) if (own != null) throw new SetupFailure($"{stub} already has a payload; use the bare installer program");
                var watch = Stopwatch.StartNew();
                PayloadWriter.Compose(stub, o.ComposeFolder!, o.ComposeOut!, o.Meta, o.Quality, line => System.Console.Out.WriteLine("  " + line));
                var size = new FileInfo(o.ComposeOut!).Length;
                System.Console.Out.WriteLine($"{o.ComposeOut}: {size / 1048576.0:N1} MB ({o.Meta.Files} files, {o.Meta.Bytes / 1048576.0:N1} MB unpacked) in {watch.Elapsed.TotalSeconds:N0} s");
                return 0;
            }
            var exe = o.InfoExe ?? Environment.ProcessPath!;
            using var payload = Payload.Open(exe) ?? throw new SetupFailure($"{exe} has no payload");
            if (o.Mode == "info")
            {
                foreach (var (k, v) in payload.Meta.Values) System.Console.Out.WriteLine($"{k}={v}");
                return 0;
            }
            payload.Extract(o.ExtractTo!, (done, total) => { });
            System.Console.Out.WriteLine($"unpacked {payload.Meta.Files} files to {o.ExtractTo}");
            return 0;
        }
        catch (Exception ex)
        {
            System.Console.Error.WriteLine("MCLA-Setup: " + (ex is SetupFailure ? ex.Message : ex.ToString()));
            return 1;
        }
    }

    // ---- install, update, uninstall ---------------------------------------------------------------------------------

    static int Run(Options o)
    {
        if (!o.Silent) Dialogs.WaitForDialog(3000);
        var target = Path.GetFullPath(o.Dir ?? DefaultDir()).TrimEnd('\\');
        Guard(target);
        FolderSwap.Recover(target);
        return o.Mode == "uninstall" ? Uninstall(o, target) : Install(o, target);
    }

    static void Status(Options o, string text, int? permille = null)
    {
        Log(text);
        if (o.Silent) return;
        Dialogs.SetText(text);
        if (permille != null) Dialogs.SetProgress(permille.Value);
    }

    /// Nothing is ever put over a folder of a fork checkout (the launcher built from source lives there), over a
    /// drive's root, or over the folder this program runs from.
    static void Guard(string target)
    {
        var root = Path.GetPathRoot(target);
        if (string.IsNullOrEmpty(target) || string.Equals(target + "\\", root, StringComparison.OrdinalIgnoreCase))
            throw new SetupFailure($"{target} is a drive's root; choose a folder for the launcher.");
        for (var dir = new DirectoryInfo(target); dir != null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "build-game.ps1")))
                throw new SetupFailure($"{target} is inside a copy of the game's source ({dir.FullName}), which has its own launcher. Choose another folder.");
        var self = Environment.ProcessPath;
        if (self != null && self.StartsWith(target + "\\", StringComparison.OrdinalIgnoreCase))
            throw new SetupFailure("Run MCLA-Setup.exe from outside the launcher's folder (for example from Downloads).");
    }

    static int Install(Options o, string target)
    {
        var self = Environment.ProcessPath ?? throw new SetupFailure("can't tell where this program is");
        using var payload = Payload.Open(self) ?? throw new SetupFailure("This installer has no launcher inside it. Download MCLA-Setup.exe again from the releases page.");
        var meta = payload.Meta;
        var exists = Directory.Exists(target);
        var launcherExe = Path.Combine(target, meta.Exe);
        if (exists && !File.Exists(launcherExe) && Directory.EnumerateFileSystemEntries(target).Any())
            throw new SetupFailure($"{target} exists and is not the launcher's folder. Remove it or choose another folder.");

        if (exists && o.Mode == "install" && !o.Force && Newer(Installed(target), meta.Version))
        {
            var message = $"A newer launcher ({Installed(target)}) is installed than this one ({meta.Version}). Replace it with the older one?";
            if (o.Silent || !Dialogs.YesNo(message)) { Log("declined: " + message); throw new OperationCanceledException(); }
        }

        // 1. Unpack beside the folder (the launcher may be running from it: it is not touched yet).
        var parent = Path.GetDirectoryName(target)!;
        Directory.CreateDirectory(parent);
        FolderSwap.CleanLeftovers(target);
        var staged = FolderSwap.NewStagingPath(target);
        var cancelled = () => !o.Silent && Dialogs.CancelRequested;
        try
        {
            Status(o, $"Unpacking {meta.Files} files...", 0);
            var lastPermille = -1;
            payload.Extract(staged, (done, total) =>
            {
                var permille = total > 0 ? (int)(done * 900 / total) : 0;
                if (permille != lastPermille) { lastPermille = permille; if (!o.Silent) Dialogs.SetProgress(permille); }
            }, cancelled);
            File.WriteAllText(Path.Combine(staged, "installed.txt"),
                $"version={meta.Version}\ntag={meta.Tag}\ncommit={meta.Commit}\ninstalled={DateTime.UtcNow:O}\n");
            if (!File.Exists(Path.Combine(staged, meta.Exe))) throw new SetupFailure($"The installer's payload has no {meta.Exe}.");

            // 2. The old launcher must be closed before its folder can be renamed.
            if (exists)
            {
                if (o.WaitPid != 0)
                {
                    Status(o, "Waiting for the launcher to close...", 920);
                    FolderSwap.WaitForProcess(o.WaitPid, TimeSpan.FromSeconds(90));
                }
                if (!FolderSwap.WaitUntilFree(target, TimeSpan.Zero))
                {
                    Status(o, "The launcher is open. Close it to go on...", 920);
                    if (!FolderSwap.WaitUntilFree(target, TimeSpan.FromMinutes(o.Silent || o.Mode == "update" ? 2 : 10), cancelled))
                        throw cancelled() ? new OperationCanceledException() : new SetupFailure("The launcher is still running. Close it and run MCLA-Setup.exe again.");
                }
            }
            if (cancelled()) throw new OperationCanceledException();
            if (!o.Silent) Dialogs.DisableCancel();

            // 3. Swap.
            Status(o, "Installing...", 950);
            if (exists) FolderSwap.CopyKept(target, staged, o.Keep);
            SwapRecord swap;
            try { swap = FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(o.RenameSeconds)); }
            catch (IOException ex) { throw new SetupFailure($"Windows would not let the launcher's folder be replaced ({ex.Message}). Close anything that has it open (Explorer, a virus scanner's scan) and try again."); }

            // 4. Shortcuts and the first start; the old launcher comes back if the new one doesn't start.
            if (!o.NoShortcuts) MakeShortcuts(o, target, meta);
            if (!o.NoLaunch)
            {
                Status(o, "Starting the launcher...", 980);
                var result = LaunchAndCheck(o, Path.Combine(target, meta.Exe), target);
                if (!result.Ok)
                {
                    Log("the new launcher did not start: " + result.Why);
                    // A first install has nothing to go back to: the files stay, so that the person (and the log) can
                    // see what is wrong, instead of an empty folder and shortcuts that lead nowhere.
                    if (swap.Previous == null)
                        throw new SetupFailure($"The launcher was installed in {target} but did not start ({result.Why}). Try its shortcut again; if it still fails, the log is {Path.Combine(Path.GetTempPath(), "MCLA-Setup.log")}.");
                    var restored = FolderSwap.RollBack(swap);
                    if (restored)
                    {
                        Log("the previous launcher was put back");
                        TryStart(Path.Combine(target, meta.Exe), target, o.LaunchArgs);
                        throw new SetupFailure($"The new launcher ({meta.Version}) did not start ({result.Why}), so the previous one was put back and started. Nothing was lost.");
                    }
                    throw new SetupFailure($"The previous launcher could not be put back ({result.Why}). The log is {Path.Combine(Path.GetTempPath(), "MCLA-Setup.log")}.");
                }
            }
            FolderSwap.Commit(swap);
            Status(o, "Done.", 1000);
            return 0;
        }
        finally
        {
            // A staged folder that was never swapped in (cancelled, failed) is not left behind.
            if (Directory.Exists(staged)) FolderSwap.DeleteFolder(staged, TimeSpan.FromSeconds(3));
        }
    }

    static int Uninstall(Options o, string target)
    {
        if (!Directory.Exists(target)) { Status(o, "The launcher is not installed there."); return 0; }
        if (!File.Exists(Path.Combine(target, "MclaLauncher.exe")))
            throw new SetupFailure($"{target} is not the launcher's folder; nothing was removed.");
        if (!o.Silent && !Dialogs.YesNo($"Remove the launcher from {target} and its shortcuts? Your game install and your saves are not touched."))
            throw new OperationCanceledException();
        if (o.WaitPid != 0) FolderSwap.WaitForProcess(o.WaitPid, TimeSpan.FromSeconds(90));
        Status(o, "Removing the launcher...", 300);
        if (!FolderSwap.WaitUntilFree(target, TimeSpan.FromMinutes(o.Silent ? 1 : 5)))
            throw new SetupFailure("The launcher is still running. Close it and try again.");
        RemoveShortcuts(o, target);
        var moved = target + FolderSwap.OldTag + DateTime.Now.ToString("yyyyMMdd-HHmmss");
        FolderSwap.RenameWithRetry(target, moved, TimeSpan.FromSeconds(30));
        FolderSwap.DeleteFolder(moved, TimeSpan.FromSeconds(10));
        Status(o, "The launcher was removed.", 1000);
        return 0;
    }

    // ---- shortcuts ---------------------------------------------------------------------------------------------------

    const string ShortcutName = "Midnight Club Los Angeles.lnk";

    static IEnumerable<string> ShortcutPaths(Options o)
    {
        if (o.ShortcutDir != null)
        {
            yield return Path.Combine(o.ShortcutDir, "start-menu", ShortcutName);
            yield return Path.Combine(o.ShortcutDir, "desktop", ShortcutName);
            yield break;
        }
        yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), ShortcutName);
        yield return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), ShortcutName);
    }

    static void MakeShortcuts(Options o, string target, PayloadMeta meta)
    {
        var exe = Path.Combine(target, meta.Exe);
        foreach (var lnk in ShortcutPaths(o))
        {
            try
            {
                // A shortcut of that name that starts something else (the launcher built from source has one on
                // the desktop) is not ours to replace.
                var existing = Shortcuts.ReadTarget(lnk);
                if (existing != null && !string.Equals(Path.GetFullPath(existing), exe, StringComparison.OrdinalIgnoreCase)
                    && File.Exists(existing))
                {
                    Log($"left {lnk} alone: it starts {existing}");
                    continue;
                }
                var made = Shortcuts.Create(lnk, exe, target, Path.Combine(target, "launcher.ico"),
                    "Set up, update and play the Midnight Club: Los Angeles recompilation");
                Log(made ? $"shortcut {lnk}" : $"could not make {lnk}");
            }
            catch (Exception ex) { Log($"shortcut {lnk}: {ex.Message}"); }
        }
    }

    static void RemoveShortcuts(Options o, string target)
    {
        var exe = Path.Combine(target, "MclaLauncher.exe");
        foreach (var lnk in ShortcutPaths(o))
        {
            try
            {
                var existing = Shortcuts.ReadTarget(lnk);
                if (existing != null && string.Equals(Path.GetFullPath(existing), exe, StringComparison.OrdinalIgnoreCase)) { File.Delete(lnk); Log($"removed {lnk}"); }
            }
            catch (Exception ex) { Log($"shortcut {lnk}: {ex.Message}"); }
        }
    }

    // ---- starting the launcher ------------------------------------------------------------------------------------------

    sealed record Started(bool Ok, string Why);

    static Process? TryStart(string exe, string workingDirectory, string? arguments)
    {
        try
        {
            var psi = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = workingDirectory };
            if (!string.IsNullOrEmpty(arguments)) psi.Arguments = arguments;
            return Process.Start(psi);
        }
        catch (Exception ex) { Log($"could not start {exe}: {ex.Message}"); return null; }
    }

    /// Starts the launcher and waits until it has come up: a window, still there 3 s later (or, for tests,
    /// "alive:N": still running after N seconds). A process that ends first, or has no window after 40 s, did not start.
    static Started LaunchAndCheck(Options o, string exe, string workingDirectory)
    {
        var process = TryStart(exe, workingDirectory, o.LaunchArgs);
        if (process == null) return new Started(false, "Windows would not start it");
        var watch = Stopwatch.StartNew();
        DateTime? windowAt = null;
        var aliveSeconds = o.Health.StartsWith("alive:") && double.TryParse(o.Health[6..], out var s) ? s : (double?)null;
        while (watch.Elapsed < TimeSpan.FromSeconds(40))
        {
            process.Refresh();
            if (process.HasExited) return new Started(false, $"it ended with code {process.ExitCode} after {watch.Elapsed.TotalSeconds:N1} s");
            if (aliveSeconds != null)
            {
                if (watch.Elapsed.TotalSeconds >= aliveSeconds) return new Started(true, "");
            }
            else
            {
                if (windowAt == null && process.MainWindowHandle != 0) windowAt = DateTime.UtcNow;
                if (windowAt != null && DateTime.UtcNow - windowAt >= TimeSpan.FromSeconds(3)) return new Started(true, "");
            }
            Thread.Sleep(100);
        }
        try { process.Kill(entireProcessTree: true); } catch (Exception) { }
        return new Started(false, "it showed no window in 40 seconds");
    }

    // ---- versions --------------------------------------------------------------------------------------------------------

    static string Installed(string target)
    {
        try
        {
            foreach (var line in File.ReadAllLines(Path.Combine(target, "installed.txt")))
                if (line.StartsWith("version=")) return line[8..].Trim();
        }
        catch (Exception) { }
        return "";
    }

    static bool Newer(string installed, string offered)
    {
        static Version? Parse(string text) => Version.TryParse(text.Split('-', '+')[0].TrimStart('v'), out var v) ? v : null;
        var a = Parse(installed);
        var b = Parse(offered);
        return a != null && b != null && a > b;
    }
}

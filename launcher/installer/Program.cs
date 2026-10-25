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
//   --ready-file <path>   the new launcher is given this path (MCLA_READY_FILE) and has started when it writes the file
//   --import-pointer <install.json>   use this file as the new folder's install.json when it has none (a launcher
//                         that was unpacked from a zip moves into %LOCALAPPDATA%\Programs\MCLA)
//   --launch-args <text>  arguments for the launcher (tests)
//   --health-timeout <s>  how long the start check waits (default 90: the first start of 480 new files can wait for a virus scanner)
//   --rename-timeout <s>  how long to wait for Windows to let the old folder be renamed (default 60; tests)
//   --shortcut-dir <dir>  put the shortcuts under this folder instead of the Start menu and desktop (tests)
// The environment variables MCLA_SETUP_DIR and MCLA_SETUP_SHORTCUT_DIR stand for --dir and --shortcut-dir when those are not given (a
// test that starts the real launcher, which starts this program, sets them so that nothing lands in the real profile).
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
    public string? Dir, ShortcutDir, Stub, ComposeFolder, ComposeOut, ExtractTo, InfoExe, LaunchArgs, ReadyFile, ImportPointer;
    public bool Silent, NoLaunch, NoShortcuts, Force;
    public int WaitPid, RenameSeconds = 60, HealthSeconds = 90;
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
            catch (OperationCanceledException) { Log("cancelled"); RestartOldLauncher(); exit = 2; }
            catch (SetupFailure ex) { Log("FAILED: " + ex.Message); RestartOldLauncher(); Fail(o, ex.Message); exit = 1; }
            catch (Exception ex) { Log("FAILED: " + ex); RestartOldLauncher(); Fail(o, "Something went wrong: " + ex.Message + "\n\nThe log is " + Path.Combine(Path.GetTempPath(), "MCLA-Setup.log")); exit = 1; }
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
                case "--health-timeout": o.HealthSeconds = int.TryParse(Next(), out var ht) && ht > 0 ? ht : throw new SetupFailure("--health-timeout needs seconds"); break;
                case "--rename-timeout": o.RenameSeconds = int.TryParse(Next(), out var rt) && rt > 0 ? rt : throw new SetupFailure("--rename-timeout needs seconds"); break;
                case "--launch-args": o.LaunchArgs = Next(); break;
                case "--ready-file": o.ReadyFile = Path.GetFullPath(Next()); break;
                case "--import-pointer": o.ImportPointer = Path.GetFullPath(Next()); break;
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
        // For tests that start the real launcher (which starts the installer): where Setup puts things, without the installer's own options.
        o.Dir ??= Environment.GetEnvironmentVariable("MCLA_SETUP_DIR") is { Length: > 0 } dir ? dir : null;
        o.ShortcutDir ??= Environment.GetEnvironmentVariable("MCLA_SETUP_SHORTCUT_DIR") is { Length: > 0 } shortcuts ? shortcuts : null;
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

    /// Set during an update from the moment the old launcher has quit until the swap is made (or the old one is back): the old
    /// launcher is started again if the update fails before the folders change, since the person was left with no window.
    static string? restartOldExe, restartOldDir, restartOldArguments;

    static void RestartOldLauncher()
    {
        var exe = restartOldExe;
        var dir = restartOldDir;
        restartOldExe = null;
        if (exe == null || dir == null || !File.Exists(exe)) return;
        try
        {
            if (FolderSwap.ProcessesUnder(dir).Count > 0) return;
            Log("starting the launcher that was there before");
            TryStart(exe, dir, restartOldArguments);
        }
        catch (Exception ex) { Log("could not start the previous launcher: " + ex.Message); }
    }

    static int Run(Options o)
    {
        if (!o.Silent) Dialogs.WaitForDialog(3000);
        // From the start: whatever refuses or fails before the folders change, the launcher that quit to hand over is started again.
        if (o.Mode == "update")
        {
            restartOldDir = Path.GetFullPath(o.Dir ?? DefaultDir()).TrimEnd('\\');
            restartOldExe = Path.Combine(restartOldDir, "MclaLauncher.exe");
            restartOldArguments = o.LaunchArgs;
        }
        var target = Guard(Path.GetFullPath(o.Dir ?? DefaultDir()));
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

    /// The folders Setup never uses as the launcher's: the user's own (profile, Desktop, Documents, Downloads, Windows, Program
    /// Files, AppData and the Programs folder itself), and any folder that holds one of them.
    static IEnumerable<string> ProtectedFolders()
    {
        string Known(Environment.SpecialFolder f) => Environment.GetFolderPath(f);
        var profile = Known(Environment.SpecialFolder.UserProfile);
        var local = Known(Environment.SpecialFolder.LocalApplicationData);
        foreach (var path in new[]
        {
            profile, Known(Environment.SpecialFolder.DesktopDirectory), Known(Environment.SpecialFolder.MyDocuments),
            profile.Length > 0 ? Path.Combine(profile, "Downloads") : "", Known(Environment.SpecialFolder.Windows),
            Known(Environment.SpecialFolder.ProgramFiles), Known(Environment.SpecialFolder.ProgramFilesX86),
            Known(Environment.SpecialFolder.ApplicationData), local, local.Length > 0 ? Path.Combine(local, "Programs") : "",
            Known(Environment.SpecialFolder.System),
        })
            if (path.Length > 0) yield return Path.GetFullPath(path).TrimEnd('\\');
    }

    /// Nothing is ever put over a folder of a fork checkout (the launcher built from source lives there), over a drive's
    /// or share's root, over the user's own folders (or a folder that holds them), over a link, or over the folder this program
    /// runs from. Returns the folder without a trailing backslash.
    static string Guard(string full)
    {
        var root = (Path.GetPathRoot(full) ?? "").TrimEnd('\\');
        var target = full.TrimEnd('\\');
        if (target.Length == 0 || target.Equals(root, StringComparison.OrdinalIgnoreCase) || target.Length <= 2)
            throw new SetupFailure($"{full} is a drive's or share's root; choose a folder for the launcher.");
        foreach (var protectedFolder in ProtectedFolders())
            if (protectedFolder.Equals(target, StringComparison.OrdinalIgnoreCase) || protectedFolder.StartsWith(target + "\\", StringComparison.OrdinalIgnoreCase))
                throw new SetupFailure($"{target} is one of your own folders, or holds one; choose a folder of its own for the launcher.");
        if (Directory.Exists(target) && FolderSwap.IsLink(target))
            throw new SetupFailure($"{target} is a link to another folder; choose the folder itself.");
        for (var dir = new DirectoryInfo(target); dir != null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "build-game.ps1")))
                throw new SetupFailure($"{target} is inside a copy of the game's source ({dir.FullName}), which has its own launcher. Choose another folder.");
        var self = Environment.ProcessPath;
        if (self != null && self.StartsWith(target + "\\", StringComparison.OrdinalIgnoreCase))
            throw new SetupFailure("Run MCLA-Setup.exe from outside the launcher's folder (for example from Downloads).");
        return target;
    }

    /// Refuses (with the reason, in plain words) to replace or remove a folder that is not wholly Setup's: one without Setup's
    /// installed.txt, one that holds things Setup did not unpack, or one that holds the install its install.json names.
    /// Returns what Setup unpacked there (the list in the folder, or `fallback()` for a folder made before there was one).
    static List<string> CheckOwned(string target, string verb, IEnumerable<string>? keep, Func<List<string>> fallback)
    {
        if (!File.Exists(Path.Combine(target, FolderSwap.OwnerMarker)))
            throw new SetupFailure($"{target} was not made by this setup (it has no {FolderSwap.OwnerMarker}), so it is not {verb}. Choose another folder or empty this one.");
        if (FolderSwap.InstallRootInside(target) is { } inside)
            throw new SetupFailure($"Your install ({inside}) is inside the launcher's folder {target}, which would take it along. Nothing was {(verb.StartsWith("replaced") ? "replaced" : "removed")}. Move the launcher's folder or the install first.");
        var owned = FolderSwap.ReadManifest(target) ?? fallback();
        if (verb.StartsWith("replaced"))
        {
            // At any depth: a file of the player's in a folder the payload also has would be deleted with the old folder.
            var foreign = FolderSwap.ForeignFiles(target, owned, keep);
            if (foreign.Count > 0)
                throw new SetupFailure($"{target} holds things the setup did not put there ({string.Join(", ", foreign.Take(5))}{(foreign.Count > 5 ? ", ..." : "")}), and replacing the folder would delete them. Move them out and run the setup again.");
        }
        return owned;
    }

    /// Where to find this program again, for the messages that say to run it (an update's copy is in a folder the player never saw).
    static string WhereTheInstallerIs => $" This installer is {Environment.ProcessPath}; a new copy is on {ReleasesPage}.";

    const string ReleasesPage = "https://github.com/RenanQueiroz/mcla-recomp/releases";

    static int Install(Options o, string target)
    {
        var self = Environment.ProcessPath ?? throw new SetupFailure("can't tell where this program is");
        using var payload = Payload.Open(self) ?? throw new SetupFailure("This installer has no launcher inside it. Download MCLA-Setup.exe again from the releases page.");
        var meta = payload.Meta;
        var exists = Directory.Exists(target) && Directory.EnumerateFileSystemEntries(target).Any();
        if (o.Mode == "update" && !exists) throw new SetupFailure($"There is no launcher in {target} to update.");
        if (exists) CheckOwned(target, "replaced", o.Keep, payload.ListPaths);

        if (exists && o.Mode == "install" && !o.Force && Newer(Installed(target), meta.Version))
        {
            var message = $"A newer launcher ({Installed(target)}) is installed than this one ({meta.Version}). Replace it with the older one?";
            if (o.Silent || !Dialogs.YesNo(message)) { Log("declined: " + message); throw new OperationCanceledException(); }
        }

        // 1. Unpack beside the folder (the launcher may be running from it: it is not touched yet).
        var parent = Path.GetDirectoryName(target)!;
        Directory.CreateDirectory(parent);
        FolderSwap.CleanLeftovers(target);
        var staged = FolderSwap.CreateStaging(target);
        var keepStaged = false;
        var cancelled = () => !o.Silent && Dialogs.CancelRequested;
        try
        {
            Status(o, $"Unpacking {meta.Files} files...", 0);
            var lastPermille = -1;
            var unpacked = payload.Extract(staged, (done, total) =>
            {
                var permille = total > 0 ? (int)(done * 900 / total) : 0;
                if (permille != lastPermille) { lastPermille = permille; if (!o.Silent) Dialogs.SetProgress(permille); }
            }, cancelled);
            if (!File.Exists(Path.Combine(staged, meta.Exe))) throw new SetupFailure($"The installer's payload has no {meta.Exe}.");
            File.WriteAllLines(Path.Combine(staged, FolderSwap.ManifestName), unpacked);
            File.WriteAllText(Path.Combine(staged, FolderSwap.OwnerMarker),
                $"version={meta.Version}\ntag={meta.Tag}\ncommit={meta.Commit}\ninstalled={DateTime.UtcNow:O}\n");

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
                // What the launcher wrote while it ran (its pointer) is looked at now, with the folder at rest.
                CheckOwned(target, "replaced", o.Keep, payload.ListPaths);
            }
            if (cancelled()) throw new OperationCanceledException();
            if (!o.Silent) Dialogs.DisableCancel();

            // 3. Swap.
            Status(o, "Installing...", 950);
            if (exists) FolderSwap.CopyKept(target, staged, o.Keep);
            // A pointer to an install made elsewhere (a launcher that was unpacked from a zip and moves here): used
            // only when the folder has none of its own.
            if (o.ImportPointer != null && File.Exists(o.ImportPointer) && !File.Exists(Path.Combine(staged, "install.json")))
                File.Copy(o.ImportPointer, Path.Combine(staged, "install.json"));
            SwapRecord swap;
            try { swap = FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(o.RenameSeconds)); }
            catch (SwapFailedException ex)
            {
                // The staged folder stays when the old one could not be put back: it is the launcher, until the next run repairs the folder.
                keepStaged = !ex.OldFolderIsBack;
                throw new SetupFailure(ex.OldFolderIsBack ? ex.Message : ex.Message + WhereTheInstallerIs);
            }
            catch (IOException ex) { throw new SetupFailure($"Windows would not let the launcher's folder be replaced ({ex.Message}). Close anything that has it open (Explorer, a virus scanner's scan) and try again."); }
            restartOldExe = null; // the folders changed: from here the way back is RollBack's

            // 4. Shortcuts and the first start; the old launcher comes back if the new one doesn't start.
            // An update renews the shortcuts that are there; it never makes one (the person may have deleted it).
            if (!o.NoShortcuts) MakeShortcuts(o, target, meta, createMissing: o.Mode != "update");
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
                    bool restored;
                    try { restored = FolderSwap.RollBack(swap); }
                    catch (SwapFailedException ex) { throw new SetupFailure($"The new launcher did not start ({result.Why}). {ex.Message}{WhereTheInstallerIs}"); }
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
            if (o.ReadyFile != null) { try { File.Delete(o.ReadyFile); } catch (Exception) { } }
            Status(o, "Done.", 1000);
            return 0;
        }
        finally
        {
            // A staged folder that was never swapped in (cancelled, failed) is not left behind, unless it is all there is of the launcher.
            if (!keepStaged && Directory.Exists(staged)) FolderSwap.DeleteFolder(staged, TimeSpan.FromSeconds(3));
        }
    }

    static int Uninstall(Options o, string target)
    {
        if (!Directory.Exists(target)) { Status(o, "The launcher is not installed there."); return 0; }
        using var payload = Payload.Open(Environment.ProcessPath ?? "");
        var owned = CheckOwned(target, "removed", o.Keep, () => payload?.ListPaths() ?? throw new SetupFailure($"{target} has no list of its files and this installer has no launcher inside it to take one from."));
        if (!o.Silent && !Dialogs.YesNo($"Remove the launcher from {target} and its shortcuts? Your game install and your saves are not touched."))
            throw new OperationCanceledException();
        if (o.WaitPid != 0) FolderSwap.WaitForProcess(o.WaitPid, TimeSpan.FromSeconds(90));
        Status(o, "Removing the launcher...", 300);
        if (!FolderSwap.WaitUntilFree(target, TimeSpan.FromMinutes(o.Silent ? 1 : 5)))
            throw new SetupFailure("The launcher is still running. Close it and try again.");
        RemoveShortcuts(o, target);
        // Only what Setup unpacked: anything else in the folder stays, and so does the folder.
        var left = FolderSwap.DeleteOwned(target, owned, TimeSpan.FromSeconds(10));
        Status(o, left.Count == 0 ? "The launcher was removed." : $"The launcher was removed; {string.Join(", ", left.Take(5))} in {target} is not the setup's and was left.", 1000);
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

    static void MakeShortcuts(Options o, string target, PayloadMeta meta, bool createMissing)
    {
        var exe = Path.Combine(target, meta.Exe);
        foreach (var lnk in ShortcutPaths(o))
        {
            try
            {
                // A shortcut file that is there is renewed only when it starts this launcher; one that starts something else
                // (the launcher built from source has one on the desktop), or that can't be read, is not ours to replace.
                if (File.Exists(lnk))
                {
                    var existing = Shortcuts.ReadTarget(lnk);
                    if (existing == null || !string.Equals(Path.GetFullPath(existing), exe, StringComparison.OrdinalIgnoreCase))
                    {
                        Log($"left {lnk} alone: it starts {existing ?? "something that could not be read"}");
                        continue;
                    }
                }
                else if (!createMissing) continue;
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

    static Process? TryStart(string exe, string workingDirectory, string? arguments, string? readyFile = null)
    {
        try
        {
            var psi = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = workingDirectory };
            if (!string.IsNullOrEmpty(arguments)) psi.Arguments = arguments;
            if (readyFile != null) psi.Environment["MCLA_READY_FILE"] = readyFile;
            return Process.Start(psi);
        }
        catch (Exception ex) { Log($"could not start {exe}: {ex.Message}"); return null; }
    }

    /// Starts the launcher and waits until it has come up: a window, still there 3 s later (or, for tests,
    /// "alive:N": still running after N seconds). A process that ends first, or has no window within the health timeout
    /// (90 s: the first start of 480 new files can wait for a virus scanner), did not start.
    /// With --ready-file the launcher is told the file's path (the MCLA_READY_FILE variable) and writes it from its window's
    /// thread once the window has been up for a while; it has come up when the file is there and the process has a window (or,
    /// for tests, "alive:N" says it need not).
    static Started LaunchAndCheck(Options o, string exe, string workingDirectory)
    {
        if (o.ReadyFile != null) { try { File.Delete(o.ReadyFile); } catch (Exception) { } }
        var process = TryStart(exe, workingDirectory, o.LaunchArgs, o.ReadyFile);
        if (process == null) return new Started(false, "Windows would not start it");
        var watch = Stopwatch.StartNew();
        DateTime? windowAt = null;
        var aliveSeconds = o.Health.StartsWith("alive:") && double.TryParse(o.Health[6..], out var s) ? s : (double?)null;
        while (watch.Elapsed < TimeSpan.FromSeconds(o.HealthSeconds))
        {
            process.Refresh();
            if (process.HasExited) return new Started(false, $"it ended with code {process.ExitCode} after {watch.Elapsed.TotalSeconds:N1} s");
            if (o.ReadyFile != null)
            {
                if (File.Exists(o.ReadyFile) && (aliveSeconds != null || process.MainWindowHandle != 0)) return new Started(true, "");
            }
            else if (aliveSeconds != null)
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
        return new Started(false, o.ReadyFile != null ? $"it did not say it was ready, with a window, in {o.HealthSeconds} seconds" : $"it showed no window in {o.HealthSeconds} seconds");
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

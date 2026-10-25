// Updates of an install made by the setup wizard (Installed mode). The launcher of a git clone follows the fork's
// main (Backend.cs, Repo); this follows what the player chose:
//
//   release  the fork's latest published release (release.json, read from GitHub): its tag is fetched and checked
//            out, the SDK bundle and the tools the release names are downloaded and checked, and the game is built the
//            way the first build was (make_pc.ps1 swaps the bundle when it is another one, and Ninja builds only what
//            changed)
//   main     "Latest changes (main, untested)", from More: origin/main, built with the SDK that is installed; refused
//            when main changes the SDK's patches (a prebuilt SDK comes with a release only)
//
// The order of an update, so that a stop, a crash or a power cut at any point leaves something the next start can finish
// or build: everything is downloaded and checked first (nothing in the install changes); the update is then written into
// install.json (Pending, with the new tools that were unpacked beside the old ones but are not yet the tools in use); the checkout
// moves; build-options.json names the new SDK bundle; then the new tools become the tools in use, the build folder forgets what it
// was configured with (CMakeCache.txt and build.ninja name the tools that made it) and the folders of the tools replaced go.
// A stop at any point leaves a checkout, a bundle and tools that belong together (before the move: all old; after it, a start finds
// Pending and finishes the rest, RepairPendingAsync), and a plain Rebuild builds the new version.
//
// The window drives this (LauncherApp), and so does the check harness (dev/wizard-check: update-run).
namespace MclaLauncher.Setup;

/// Where an update would take the install.
sealed record UpdateTarget(string Ref, string Commit, string Label, ReleaseInfo? Release, ReleaseSource? Source, bool IsMain, bool SwitchBack);

/// Which line an install follows, kept in its install.json (and the pointer beside the exe).
static class InstallChannel
{
    public const string Release = "release";
    public const string Main = "main";

    public static string Read(string installRoot) =>
        InstallRecord.Load(Path.Combine(installRoot, InstallLayout.RecordName))?.Channel == Main ? Main : Release;

    /// Writes the channel into the install's record and the pointer beside the exe (when that names this install). Takes the
    /// folder's lock for it, so that it never interleaves with an update or a setup writing the same record: a SetupException
    /// (in words) when one is running.
    public static void Write(string installRoot, string exeDirectory, string channel)
    {
        using var folderLock = InstallLock.Acquire(installRoot);
        var file = Path.Combine(installRoot, InstallLayout.RecordName);
        if (InstallRecord.Load(file) is not { } record) return;
        var changed = record with { Channel = channel };
        changed.Save(file);
        SavePointer(changed, exeDirectory);
    }

    internal static void SavePointer(InstallRecord record, string exeDirectory)
    {
        var pointer = InstallRecord.PointerFile(exeDirectory);
        try
        {
            if (InstallRecord.Load(pointer) is { } existing && SetupEngine.SamePath(existing.InstallRoot, record.InstallRoot))
                record.Save(pointer);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }
}

static class UpdateResolver
{
    static string FirstLine(ProcResult r) =>
        (r.Error + "\n" + r.Output).Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 0) ?? $"exit code {r.ExitCode}";

    /// The commit the install would move to, or null when the player asked not to look (fetch false). Fetches the
    /// tag (or main) into the checkout, but moves nothing. Throws SetupException with a plain message when it can't.
    public static async Task<UpdateTarget?> ResolveAsync(string repo, string channel, bool fetch, CancellationToken ct)
    {
        if (!fetch) return null;
        if (channel == InstallChannel.Main)
        {
            var got = await RunGit(repo, "fetch", "--quiet", "--no-tags", "origin", "+refs/heads/main:refs/remotes/origin/main");
            if (got.ExitCode != 0) throw new SetupException($"Couldn't get the latest changes from GitHub: {FirstLine(got)}");
            var main = (await RunGit(repo, "rev-parse", "--verify", "refs/remotes/origin/main")).Output.Trim();
            return new UpdateTarget("origin/main", main, "the latest changes (main)", null, null, true, false);
        }

        var source = ReleaseSource.FromOverride(DevOptions.Current.ReleaseSource);
        var release = await source.LoadAsync(ct);
        var tagRef = $"refs/tags/{release.Tag}";
        var fetched = await RunGit(repo, "fetch", "--quiet", "--no-tags", "origin", $"+{tagRef}:{tagRef}");
        if (fetched.ExitCode != 0) throw new SetupException($"Couldn't get release {release.Tag} from GitHub: {FirstLine(fetched)}");
        var commit = (await RunGit(repo, "rev-parse", "--verify", $"{release.Tag}^{{commit}}")).Output.Trim();
        // As the setup does: what is built is what the release says, even if the tag was moved.
        if (commit != release.Commit)
            throw new SetupException($"Release {release.Tag}'s tag points at {(commit.Length >= 8 ? commit[..8] : "nothing")}, not at the commit {release.Commit[..8]} the release names, so it was not offered.");
        var head = (await RunGit(repo, "rev-parse", "HEAD")).Output.Trim();
        // On newer code than the release (the player followed main and now follows the releases again).
        var back = commit != head && (await RunGit(repo, "merge-base", "--is-ancestor", commit, "HEAD")).ExitCode == 0;
        return new UpdateTarget(release.Tag, commit, $"release {release.Tag}", release, source, false, back);
    }

    static Task<ProcResult> RunGit(string repo, params string[] args) =>
        Proc.RunAsync("git", ["-C", repo, .. args], TimeSpan.FromSeconds(60));
}

/// An update that has been prepared (what it needs is downloaded and checked, the checkout has moved): the build to run,
/// and the folder's lock, held until the build is over.
sealed class UpdateSession : IDisposable
{
    readonly InstallLock folderLock;
    public BuildRunOptions Options { get; }
    public UpdateTarget Target { get; }
    /// Something the game is made of changed (files outside the launcher and the documentation, a new SDK bundle or
    /// tool): only then are the shaders prepared again after the build.
    public bool GameChanged { get; }
    /// The last steps of the update (switching to the new tools) did not work after the checkout had moved (a file of an old tool held
    /// open): the build goes on, and they are tried again when it is over, or at the next start (the update stays recorded).
    public bool ToolSwitchPending { get; }

    internal UpdateSession(InstallLock folderLock, BuildRunOptions options, UpdateTarget target, bool gameChanged, bool toolSwitchPending = false)
    {
        this.folderLock = folderLock;
        Options = options;
        Target = target;
        GameChanged = gameChanged;
        ToolSwitchPending = toolSwitchPending;
    }

    public void Dispose() => folderLock.Dispose();
}

static class UpdateEngine
{
    /// Takes the folder's lock and gets ready: for a release, the tools and the SDK bundle it names are downloaded
    /// and checked (what is already there is not fetched again) and there is room for them; for main, the SDK's patches
    /// are compared. Then, in the order the header says, the update is recorded and the checkout moves. The build
    /// itself is `build-game.ps1` with the options in the session. Throws SetupException (in words) or an IO error; the
    /// lock is let go either way, and a move that did not happen leaves nothing recorded.
    public static async Task<UpdateSession> PrepareAsync(InstallLayout layout, UpdateTarget target, Action<string> log, IProgress<string> progress, CancellationToken ct)
    {
        var folderLock = InstallLock.Acquire(layout.Root);
        // What the record said before this update wrote its own (an earlier update, stopped after its move and not yet built,
        // is still waiting): it comes back if this one fails before its move.
        PendingUpdate? previous = null;
        var recorded = false;
        try
        {
            // The checkout is moved here, not by build-game.ps1 -Ref: the script of the install being updated is the old one,
            // which has no -Ref (the new one comes with the checkout). -Ref is the same step for a terminal.
            if (target.IsMain)
            {
                var changed = await Proc.RunAsync("git", ["-C", layout.Repo, "diff", "--name-only", "HEAD", target.Commit, "--", "patches/*.patch"], TimeSpan.FromSeconds(60));
                if (changed.Output.Trim().Length > 0)
                    throw new SetupException("The latest changes (main) change the ReXGlue SDK, and an install made by the setup has the SDK that came with its release (it can't build one). "
                                             + "Stay on the release, or wait for the next one.", changed.Output);
                log("main does not change the SDK's patches");
                var mainChanged = await FilesChangedAsync(layout, target);
                previous = SetPending(layout, target, []);
                recorded = true;
                await MoveToAsync(layout, target, log);
                return new UpdateSession(folderLock, new BuildRunOptions(layout.LauncherDir, layout.LogsDir, [], layout.GitDir, IsUpdate: true), target, mainChanged);
            }

            var release = target.Release!;
            var source = target.Source!;
            var bundle = release.SdkBundle!;
            var zip = Path.Combine(layout.Downloads, bundle.Asset);

            // 1. What has to be fetched, and room for it (the archives, what they unpack to, and the build).
            var toInstall = ReleaseInfo.RequiredTools.Where(name => !ToolInstaller.IsInstalled(layout.Tools, name, release.Tools[name].Sha256)).ToList();
            var needed = bundle.Size * 5 + toInstall.Sum(name => release.Tools[name].Size * 6) + (1L << 30);
            var have = PathBudget.FreeBytes(layout.Root);
            if (have < needed)
                throw new SetupException($"There is not enough room on that drive for the update: it needs about {GameChecker.FormatSize(needed)} and {GameChecker.FormatSize(have)} is free. Free some space and try again.",
                                         $"needed {needed}, free {have}");

            // 2. Downloads, all checked: nothing in the install has changed yet.
            progress.Report("Checking the tools");
            foreach (var name in ReleaseInfo.RequiredTools.Except(toInstall)) log($"{name}: already unpacked");
            foreach (var name in toInstall)
            {
                var tool = release.Tools[name];
                progress.Report($"Downloading {name}");
                var file = Path.Combine(layout.Downloads, Path.GetFileName(new Uri(tool.Url).AbsolutePath));
                await Verified.FetchAsync(name, file, tool.Size, tool.Sha256,
                    (dest, p, token) => FetchTool(source, tool, dest, p, token), new Progress<long>(_ => { }), log, ct);
            }
            progress.Report("Downloading the SDK");
            await Verified.FetchAsync("the SDK bundle", zip, bundle.Size, bundle.Sha256,
                (dest, p, token) => source.FetchAssetAsync(bundle.Asset, dest, p, token), new Progress<long>(_ => { }), log, ct);

            // 3. The new tools, unpacked beside the old ones. They are not recorded as the tools in use yet: until the checkout has
            //    moved, a build (a plain Rebuild after a stop) must still use the tools the old checkout and its SDK were built with.
            var staged = new List<PendingTool>();
            foreach (var name in toInstall)
            {
                var tool = release.Tools[name];
                progress.Report($"Unpacking {name}");
                var archive = Path.GetFileName(new Uri(tool.Url).AbsolutePath);
                var folder = await ToolInstaller.InstallAsync(layout.Tools, name, tool.Version, Path.Combine(layout.Downloads, archive), tool.Sha256,
                                                              log, new Progress<(int, int)>(), ct, record: false);
                staged.Add(new PendingTool(name, Path.GetFileName(folder), tool.Sha256, archive));
            }
            var gameChanged = toInstall.Count > 0 || BundleInUse(layout) != bundle.Sha256.ToLowerInvariant() || await FilesChangedAsync(layout, target);

            // 4. Recorded, then the checkout moves, and the build options name the bundle that goes with it.
            previous = SetPending(layout, target, staged);
            recorded = true;
            progress.Report("Moving to the release");
            await MoveToAsync(layout, target, log);

            // From here the update is recorded and the checkout is at the new version: what goes wrong is not "the update stopped"
            // but "the rest finishes later" (the next start does it, or the end of this build). A stop asked for says so in words.
            var toolSwitchPending = false;
            try
            {
                WriteOptions(layout, bundle.Asset, bundle.Sha256);

                // 5. The new tools become the tools in use, the build folder forgets the tools it was configured with, and the tools
                //    replaced go: nothing refers to them any more, so no build, now or later, runs with one of them or breaks for lack
                //    of it. The next build configures the folder again.
                progress.Report("Switching to the new tools");
                ActivateTools(layout, staged, log, ct);
            }
            catch (OperationCanceledException)
            {
                throw new SetupException("The update was stopped after the new version had been put in place. It is recorded: the next start finishes it, and Rebuild builds it.");
            }
            catch (Exception ex)
            {
                log($"the switch to the new tools is not finished ({ex.Message}); it is tried again when the build is over, or at the next start");
                toolSwitchPending = true;
            }

            string[] arguments = ["-Toolchain", "mingw", "-ToolsDir", layout.Tools, "-SdkBundle", zip, "-SdkBundleSha256", bundle.Sha256];
            return new UpdateSession(folderLock, new BuildRunOptions(layout.LauncherDir, layout.LogsDir, arguments, layout.GitDir, IsUpdate: true), target, gameChanged, toolSwitchPending);
        }
        catch
        {
            // A move that did not happen: what was recorded before this update comes back (the downloads and the unpacked tools stay,
            // checked, for a retry). A move that did happen leaves this update recorded, for the next start to finish.
            try { if (recorded && HeadOf(layout) != target.Commit) RestorePending(layout, target.Commit, previous); }
            catch (Exception) { }
            finally { folderLock.Dispose(); }
            throw;
        }
    }

    // ---- the record of an update that has begun -------------------------------------------------------------------------

    /// Writes this update's record and returns the one it replaced (null when there was none).
    static PendingUpdate? SetPending(InstallLayout layout, UpdateTarget target, IReadOnlyList<PendingTool> tools)
    {
        if (InstallRecord.Load(layout.RecordFile) is not { } record) return null;
        var bundle = target.Release?.SdkBundle;
        (record with { Pending = new PendingUpdate(target.Ref, target.Commit, target.IsMain ? "main" : target.Release!.Tag, bundle?.Asset, bundle?.Sha256, tools.ToArray()) })
            .Save(layout.RecordFile);
        return record.Pending;
    }

    /// An update that failed before its move takes its own record away, and puts back the one it had replaced; a record that
    /// is not its own (another update's) is left as it is.
    static void RestorePending(InstallLayout layout, string commit, PendingUpdate? previous)
    {
        if (InstallRecord.Load(layout.RecordFile) is { Pending: { } current } record && current.Commit == commit)
            (record with { Pending = previous }).Save(layout.RecordFile);
    }

    static string HeadOf(InstallLayout layout) =>
        Proc.RunAsync("git", ["-C", layout.Repo, "rev-parse", "HEAD"], TimeSpan.FromSeconds(30)).GetAwaiter().GetResult().Output.Trim();

    /// At the launcher's start (Installed mode), when no update, build or setup is running in the folder: an update that began and
    /// was not built (the launcher was closed, crashed or the power went) is looked at.
    ///  - The checkout never moved: nothing was changed (the tools in use are the old ones); the record is dropped, and the
    ///    new tools that were unpacked and not used are removed (their archives stay, for a retry).
    ///  - It moved: build-options.json is made to name the bundle that goes with it, the new tools become the tools in use (the
    ///    last steps of the update, ActivateTools), and if what was built is still current (a release that changed only the
    ///    launcher or documentation) the update is complete; otherwise the Rebuild that is offered completes it.
    public static async Task RepairPendingAsync(InstallLayout layout, string exeDirectory)
    {
        if (InstallRecord.Load(layout.RecordFile) is not { Pending: not null }) return;   // nothing begun (the usual case): no lock taken
        InstallLock folderLock;
        try { folderLock = InstallLock.Acquire(layout.Root); }
        catch (SetupException) { return; }   // an update (of this or another window) is running: its record is not stale
        using (folderLock)
        {
            if (InstallRecord.Load(layout.RecordFile) is not { Pending: { } pending } record) return;
            var head = (await Proc.RunAsync("git", ["-C", layout.Repo, "rev-parse", "HEAD"], TimeSpan.FromSeconds(30))).Output.Trim();
            if (head != pending.Commit)
            {
                (record with { Pending = null }).Save(layout.RecordFile);
                ToolInstaller.RemoveStaged(layout.Tools);
                RemoveSupersededTools(layout, _ => { }, CancellationToken.None);
                return;
            }
            if (pending.BundleAsset != null && pending.BundleSha256 != null && BundleInUse(layout) != pending.BundleSha256.ToLowerInvariant()
                && File.Exists(Path.Combine(layout.Downloads, pending.BundleAsset)))
                WriteOptions(layout, pending.BundleAsset, pending.BundleSha256);
            ActivateTools(layout, pending.Tools ?? [], _ => { }, CancellationToken.None);
            if (await BuiltIsCurrentAsync(layout, head)) CompletePending(layout, exeDirectory);
        }
    }

    /// The last finished build is of this commit, or of one that differs from it only in the launcher and the documentation.
    static async Task<bool> BuiltIsCurrentAsync(InstallLayout layout, string head)
    {
        var file = Path.Combine(layout.LauncherDir, "built-commit.txt");
        if (!File.Exists(file) || !File.Exists(layout.GameExe)) return false;
        var built = File.ReadAllText(file).Trim();
        if (built == head) return true;
        if (built.Length == 0) return false;
        var diff = await Proc.RunAsync("git", ["-C", layout.Repo, "diff", "--name-only", built, head, "--", ".", ":(exclude)launcher", ":(exclude)*.md"], TimeSpan.FromSeconds(60));
        return diff.ExitCode == 0 && diff.Output.Trim().Length == 0;
    }

    /// A plain Rebuild finished well after an update had begun: the install is at that update.
    public static void CompletePending(InstallLayout layout, string exeDirectory)
    {
        if (InstallRecord.Load(layout.RecordFile) is not { Pending: { } pending } record) return;
        var done = record with { ReleaseTag = pending.Tag, ReleaseCommit = pending.Commit, FinishedUtc = DateTime.UtcNow, Pending = null };
        done.Save(layout.RecordFile);
        InstallChannel.SavePointer(done, exeDirectory);
        if (pending.BundleAsset != null) RemoveOtherBundles(layout, pending.BundleAsset);
    }

    // ---- the tools ------------------------------------------------------------------------------------------------------

    /// The last steps of an update, once the checkout is at its commit: each unpacked tool becomes the tool in use (the record of
    /// tools\ names it), its archive goes, and the folders of the tools it replaced are removed. Safe to run again.
    internal static void ActivateTools(InstallLayout layout, IReadOnlyList<PendingTool> tools, Action<string> log, CancellationToken ct)
    {
        foreach (var tool in tools)
        {
            // install.json is a file anyone can edit: what it names is a tool and plain folder and file names, nothing else.
            if (!ReleaseInfo.RequiredTools.Contains(tool.Name) || !ToolInstaller.IsPlainName(tool.Folder) || !ToolInstaller.IsPlainName(tool.Archive))
            {
                log($"the record of the update names something that is not a tool of this program ({tool.Name}); it was skipped");
                continue;
            }
            // (The record names the folder and the archive's SHA-256: both are checked, so that a repackaged tool that unpacks into
            // a folder of the same name is switched too.)
            if (!ToolInstaller.IsInstalled(layout.Tools, tool.Name, tool.Sha256)
                && (ToolInstaller.IsStaged(layout.Tools, tool.Folder, tool.Sha256) || ToolInstaller.IsUnpacked(layout.Tools, tool.Folder, tool.Sha256)))
            {
                ToolInstaller.Activate(layout.Tools, tool.Name, tool.Folder, tool.Sha256);
                log($"{tool.Name}: {tool.Folder} is the one in use now");
            }
            if (ToolInstaller.IsInstalled(layout.Tools, tool.Name, tool.Sha256) && tool.Archive.Length > 0 && Path.GetFileName(tool.Archive) == tool.Archive)
            {
                var archive = Path.Combine(layout.Downloads, tool.Archive);
                if (File.Exists(archive)) Retry.Io(() => File.Delete(archive), ct: ct);
            }
        }
        ToolInstaller.RemoveStaged(layout.Tools);
        RemoveSupersededTools(layout, log, ct);
    }

    /// Removes the folders in tools\ that this program unpacked (they carry its marker) and that the record no longer names, and
    /// the temporary folders of an unpacking that was cut short. If the build folder was configured with one of them (CMakeCache.txt
    /// names the CMake, Ninja and compiler that made it), it first forgets that: CMakeCache.txt and build.ninja go, and build.ps1
    /// configures again when there is no build.ninja.
    static void RemoveSupersededTools(InstallLayout layout, Action<string> log, CancellationToken ct)
    {
        ToolInstaller.RestoreAside(layout.Tools);
        var folders = ToolInstaller.Superseded(layout.Tools);
        if (folders.Count == 0) return;
        if (BuildFolderUses(layout, folders))
        {
            foreach (var name in new[] { "CMakeCache.txt", "build.ninja" })
            {
                var file = Path.Combine(layout.BuildDir, name);
                if (File.Exists(file)) Retry.Io(() => File.Delete(file), ct: ct);
            }
            log("the build folder will be configured again (a build tool changed)");
        }
        foreach (var folder in folders) DeleteToolFolder(layout, folder, log);
    }

    /// The configured build folder names (in its CMakeCache.txt) a tool in one of these folders.
    static bool BuildFolderUses(InstallLayout layout, List<string> folders)
    {
        var cache = Path.Combine(layout.BuildDir, "CMakeCache.txt");
        try
        {
            if (!File.Exists(cache)) return File.Exists(Path.Combine(layout.BuildDir, "build.ninja"));
            var text = File.ReadAllText(cache).Replace('\\', '/');
            return folders.Any(f => text.Contains("/" + Path.GetFileName(f) + "/", StringComparison.OrdinalIgnoreCase));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return true; }
    }

    /// build-options.json as make_pc.ps1 writes it (UTF-8 with a mark), naming the update's bundle and the install's tools:
    /// what a plain build uses. It is written only after the checkout has moved, so it never names a bundle the checkout
    /// is not at.
    static void WriteOptions(InstallLayout layout, string bundleAsset, string bundleSha256)
    {
        var json = System.Text.Json.JsonSerializer.Serialize(new Dictionary<string, string>
        {
            ["toolchain"] = "mingw",
            ["toolsDir"] = layout.Tools,
            ["sdkBundle"] = Path.Combine(layout.Downloads, bundleAsset),
            ["sdkBundleSha256"] = bundleSha256,
            ["sdkDir"] = Path.Combine(layout.Repo, "third_party", "rexglue-sdk-bin"),
        }, new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
        var file = Path.Combine(layout.LauncherDir, "build-options.json");
        var temp = file + ".tmp";
        Retry.Io(() =>
        {
            File.WriteAllText(temp, json + "\r\n", new System.Text.UTF8Encoding(true));
            File.Move(temp, file, overwrite: true);
        });
    }

    static void DeleteToolFolder(InstallLayout layout, string folder, Action<string> log)
    {
        // Only a direct child of tools\ (never anything else the record might name).
        if (Directory.GetParent(Path.GetFullPath(folder))?.FullName.TrimEnd('\\') is not { } parent
            || !string.Equals(parent, Path.GetFullPath(layout.Tools).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)) return;
        // Renamed first: a file in it held open (an old tool still running) makes the rename fail with the folder whole, marker
        // included, so that it is tried again later. Once renamed it is a temporary folder (.u-*), which any later cleanup removes
        // even if the deleting is cut short; deleting it in place could leave it without its marker, and so unrecognised.
        var name = Path.GetFileName(folder);
        var aside = Path.Combine(layout.Tools, name.StartsWith(".u-", StringComparison.Ordinal) ? name : ".u-old-" + name);
        if (!string.Equals(aside, folder, StringComparison.OrdinalIgnoreCase))
        {
            SafeFolder.Delete(aside);
            Retry.Io(() => Directory.Move(folder, aside));
        }
        SafeFolder.Delete(aside);
        log($"{name} is replaced and removed");
    }

    static void RemoveOtherBundles(InstallLayout layout, string keep)
    {
        if (!Directory.Exists(layout.Downloads)) return;
        foreach (var zipFile in Directory.EnumerateFiles(layout.Downloads, "rexglue-sdk-*.zip"))
            if (!string.Equals(Path.GetFileName(zipFile), keep, StringComparison.OrdinalIgnoreCase))
                Retry.Io(() => File.Delete(zipFile));
    }

    // ---- the checkout ---------------------------------------------------------------------------------------------------

    /// Moves the checkout to the target commit (already fetched by UpdateResolver): forward, a fast-forward (what codegen
    /// rewrote is put back first, so that nothing has to be stashed or can conflict: they are build products, made again);
    /// otherwise (back to a release from main) a checkout. A merge that leaves conflicts is a failure.
    static async Task MoveToAsync(InstallLayout layout, UpdateTarget target, Action<string> log)
    {
        var head = (await Proc.RunAsync("git", ["-C", layout.Repo, "rev-parse", "HEAD"], TimeSpan.FromSeconds(30))).Output.Trim();
        if (head == target.Commit) { log($"already at {target.Label}"); return; }
        var ancestor = (await Proc.RunAsync("git", ["-C", layout.Repo, "merge-base", "--is-ancestor", head, target.Commit], TimeSpan.FromSeconds(30))).ExitCode == 0;
        await RestoreCodegenFilesAsync(layout, log);
        ProcResult moved;
        if (ancestor)
        {
            moved = await Proc.RunAsync("git", ["-c", "user.name=mcla", "-c", "user.email=mcla@localhost", "-C", layout.Repo, "merge", "--ff-only", "--autostash", target.Commit], TimeSpan.FromSeconds(120));
        }
        else
        {
            log($"{head[..7]} is not an ancestor of {target.Commit[..7]}: checking it out");
            moved = await Proc.RunAsync("git", ["-c", "advice.detachedHead=false", "-C", layout.Repo, "checkout", "--quiet", "--force", "--detach", target.Commit], TimeSpan.FromSeconds(120));
        }
        if (moved.ExitCode != 0)
            throw new SetupException("Couldn't move the install to the new version: " + (moved.Error + moved.Output).Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 0), moved.Error + moved.Output);
        // git exits 0 when an autostash could not be put back, leaving conflict markers in the files.
        var unmerged = await Proc.RunAsync("git", ["-C", layout.Repo, "ls-files", "--unmerged"], TimeSpan.FromSeconds(30));
        if (unmerged.Output.Trim().Length > 0)
            throw new SetupException("The install has changes of its own that clash with the new version, so it was not updated cleanly. Run the setup again on this folder to rebuild it.", unmerged.Output);
        log($"moved to {target.Commit[..7]}");
    }

    /// What codegen rewrites (mcla_manifest.toml, generated\rexglue.cmake) is made again by every build, so before the checkout moves it is
    /// put back to the commit's version: nothing has to be stashed, and nothing can clash with the new version of those files. A change
    /// someone made in either (the first is also the recompiler's configuration) is saved first, in kept\codegen-edits, and the log says so.
    static async Task RestoreCodegenFilesAsync(InstallLayout layout, Action<string> log)
    {
        foreach (var file in new[] { "mcla_manifest.toml", "generated/rexglue.cmake" })
        {
            // Not tracked in this checkout (an older one): nothing to save or put back.
            if ((await Proc.RunAsync("git", ["-C", layout.Repo, "ls-files", "--error-unmatch", "--", file], TimeSpan.FromSeconds(30))).ExitCode != 0) continue;
            // Line endings alone are not an edit (codegen writes LF, and git may be set to check out CRLF).
            var differs = await Proc.RunAsync("git", ["-C", layout.Repo, "diff", "--ignore-cr-at-eol", "--quiet", "HEAD", "--", file], TimeSpan.FromSeconds(30));
            if (differs.ExitCode == 1)
            {
                var copy = Path.Combine(layout.Kept, "codegen-edits", $"{file.Replace('/', '_')}.{DateTime.Now:yyyyMMdd-HHmmss}");
                Directory.CreateDirectory(Path.GetDirectoryName(copy)!);
                Retry.Io(() => File.Copy(Path.Combine(layout.Repo, file.Replace('/', '\\')), copy, overwrite: true));
                log($"{file} had changes: a copy is in {copy}");
            }
            await Proc.RunAsync("git", ["-C", layout.Repo, "checkout", "--quiet", "--", file], TimeSpan.FromSeconds(30));
        }
    }

    /// Files outside the launcher, the documentation and the release's own machinery differ between HEAD and the target.
    static async Task<bool> FilesChangedAsync(InstallLayout layout, UpdateTarget target)
    {
        var diff = await Proc.RunAsync("git", ["-C", layout.Repo, "diff", "--name-only", "HEAD", target.Commit, "--", ".",
                                              ":(exclude)launcher", ":(exclude)*.md", ":(exclude).github", ":(exclude)scripts/ci"], TimeSpan.FromSeconds(60));
        return diff.ExitCode != 0 || diff.Output.Trim().Length > 0;
    }

    /// The SHA-256 of the SDK bundle the last build used, from launcher\build-options.json ("" when there is none).
    static string BundleInUse(InstallLayout layout)
    {
        try
        {
            var file = Path.Combine(layout.LauncherDir, "build-options.json");
            if (!File.Exists(file)) return "";
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(file));
            return doc.RootElement.TryGetProperty("sdkBundleSha256", out var sha) ? (sha.GetString() ?? "").ToLowerInvariant() : "";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.Text.Json.JsonException) { return ""; }
    }

    static Task FetchTool(ReleaseSource source, ReleaseTool tool, string destination, IProgress<long> progress, CancellationToken ct)
    {
        // As in the setup: with a local release source, a tool archive of the same name beside release.json is used.
        if (source.LocalFolder is { } folder)
        {
            var name = Path.GetFileName(new Uri(tool.Url).AbsolutePath);
            foreach (var candidate in new[] { Path.Combine(folder, name), Path.Combine(folder, "tools", name) })
                if (File.Exists(candidate)) return ReleaseSource.DownloadAsync(candidate, destination, progress, ct, allowLocalFile: true);
        }
        if (!tool.Url.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
            throw new SetupException("The release names a download that isn't https; it was not used.", tool.Url);
        return ReleaseSource.DownloadAsync(tool.Url, destination, progress, ct);
    }

    /// After a build that worked: the record says which release the install is at (and nothing is pending), and the older bundle
    /// zips are removed (for a release update; never for main).
    public static void Finish(InstallLayout layout, string exeDirectory, UpdateSession session)
    {
        var target = session.Target;
        if (session.ToolSwitchPending && InstallRecord.Load(layout.RecordFile) is { Pending: { } waiting })
        {
            // The build is over, so the file that held an old tool's folder is probably free. If it still is not, the update stays
            // recorded (the options are written, the build is done): the next start finishes it.
            try { ActivateTools(layout, waiting.Tools ?? [], _ => { }, CancellationToken.None); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return; }
        }
        if (InstallRecord.Load(layout.RecordFile) is { } record)
        {
            var updated = record with
            {
                ReleaseTag = target.IsMain ? "main" : target.Release!.Tag,
                ReleaseCommit = target.Commit,
                FinishedUtc = DateTime.UtcNow,
                Pending = null,
            };
            updated.Save(layout.RecordFile);
            InstallChannel.SavePointer(updated, exeDirectory);
        }
        if (!target.IsMain && target.Release?.SdkBundle is { } bundle) RemoveOtherBundles(layout, bundle.Asset);
    }
}

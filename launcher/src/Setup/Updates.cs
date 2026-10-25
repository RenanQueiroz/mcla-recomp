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
// install.json (Pending); a replaced build tool takes the build folder's CMakeCache.txt and build.ninja with it, and the folders of the
// tools it replaces go (nothing refers to them any more); the checkout moves; build-options.json names the new SDK bundle.
// A plain Rebuild after that builds the new version, and a start that finds Pending repairs the options (RepairPendingAsync).
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

    internal UpdateSession(InstallLock folderLock, BuildRunOptions options, UpdateTarget target, bool gameChanged)
    {
        this.folderLock = folderLock;
        Options = options;
        Target = target;
        GameChanged = gameChanged;
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
                SetPending(layout, target);
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

            // 3. The new tools, unpacked beside the old ones (which keep working until the update is recorded).
            var oldFolders = new List<string>();
            foreach (var name in toInstall)
            {
                var tool = release.Tools[name];
                var oldFolder = ToolInstaller.FolderOf(layout.Tools, name);
                progress.Report($"Unpacking {name}");
                var file = Path.Combine(layout.Downloads, Path.GetFileName(new Uri(tool.Url).AbsolutePath));
                var folder = await ToolInstaller.InstallAsync(layout.Tools, name, tool.Version, file, tool.Sha256, log, new Progress<(int, int)>(), ct);
                Retry.Io(() => File.Delete(file), ct: ct);
                if (oldFolder != null && !SetupEngine.SamePath(oldFolder, folder)) oldFolders.Add(oldFolder);
            }
            var gameChanged = toInstall.Count > 0 || BundleInUse(layout) != bundle.Sha256.ToLowerInvariant() || await FilesChangedAsync(layout, target);

            // 4. Recorded, and the build folder let go of the tools it was made with: a configured build folder names the CMake
            //    and Ninja (and compiler) that made it, and the folders of the tools just replaced go now, so that no build, now
            //    or later, runs with one of them or breaks for lack of it. The next build configures the folder again.
            SetPending(layout, target);
            if (toInstall.Any(name => name != "mingit"))
            {
                // build.ps1 configures when there is no build.ninja (it reads the cache only when there is one), and CMake then finds
                // the tools again because there is no cache to remember the old ones.
                var forgotten = false;
                foreach (var name in new[] { "CMakeCache.txt", "build.ninja" })
                {
                    var file = Path.Combine(layout.BuildDir, name);
                    if (File.Exists(file)) { Retry.Io(() => File.Delete(file), ct: ct); forgotten = true; }
                }
                if (forgotten) log("the build folder will be configured again (a build tool changed)");
            }
            foreach (var old in oldFolders) DeleteToolFolder(layout, old, log);

            // 5. The checkout moves, and the build options name the bundle that goes with it.
            progress.Report("Moving to the release");
            await MoveToAsync(layout, target, log);
            WriteOptions(layout, bundle.Asset, bundle.Sha256);
            string[] arguments = ["-Toolchain", "mingw", "-ToolsDir", layout.Tools, "-SdkBundle", zip, "-SdkBundleSha256", bundle.Sha256];
            return new UpdateSession(folderLock, new BuildRunOptions(layout.LauncherDir, layout.LogsDir, arguments, layout.GitDir, IsUpdate: true), target, gameChanged);
        }
        catch
        {
            // A move that did not happen leaves no update recorded (the downloads stay: they are checked, and kept for a retry).
            try { if (HeadOf(layout) != target.Commit) ClearPending(layout); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or TimeoutException) { }
            folderLock.Dispose();
            throw;
        }
    }

    // ---- the record of an update that has begun -------------------------------------------------------------------------

    static void SetPending(InstallLayout layout, UpdateTarget target)
    {
        if (InstallRecord.Load(layout.RecordFile) is not { } record) return;
        var bundle = target.Release?.SdkBundle;
        (record with { Pending = new PendingUpdate(target.Ref, target.Commit, target.IsMain ? "main" : target.Release!.Tag, bundle?.Asset, bundle?.Sha256) })
            .Save(layout.RecordFile);
    }

    static void ClearPending(InstallLayout layout)
    {
        if (InstallRecord.Load(layout.RecordFile) is { Pending: not null } record) (record with { Pending = null }).Save(layout.RecordFile);
    }

    static string HeadOf(InstallLayout layout) =>
        Proc.RunAsync("git", ["-C", layout.Repo, "rev-parse", "HEAD"], TimeSpan.FromSeconds(30)).GetAwaiter().GetResult().Output.Trim();

    /// At the launcher's start (Installed mode): an update that began and was not built (the launcher was closed, crashed or
    /// the power went) is looked at. If the checkout is at its commit, build-options.json is made to name the bundle that goes
    /// with it (the move and the options are two steps), so that a plain Rebuild builds it; if the checkout never moved, nothing
    /// was changed and the record is dropped.
    public static async Task RepairPendingAsync(InstallLayout layout)
    {
        if (InstallRecord.Load(layout.RecordFile) is not { Pending: { } pending }) return;
        var head = (await Proc.RunAsync("git", ["-C", layout.Repo, "rev-parse", "HEAD"], TimeSpan.FromSeconds(30))).Output.Trim();
        if (head != pending.Commit) { ClearPending(layout); return; }
        if (pending.BundleAsset != null && pending.BundleSha256 != null && BundleInUse(layout) != pending.BundleSha256.ToLowerInvariant()
            && File.Exists(Path.Combine(layout.Downloads, pending.BundleAsset)))
            WriteOptions(layout, pending.BundleAsset, pending.BundleSha256);
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
        SafeFolder.Delete(folder);
        log($"{Path.GetFileName(folder)} is replaced and removed");
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
        ProcResult moved;
        if (ancestor)
        {
            await Proc.RunAsync("git", ["-C", layout.Repo, "checkout", "--quiet", "--", "mcla_manifest.toml", "generated/rexglue.cmake"], TimeSpan.FromSeconds(30));
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

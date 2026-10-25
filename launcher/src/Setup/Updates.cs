// Updates of an install made by the setup wizard (Installed mode). The launcher of a git clone follows the fork's
// main (Backend.cs, Repo); this follows what the player chose:
//
//   release  the fork's latest published release (release.json, read from GitHub): its tag is fetched and checked
//            out (build-game.ps1 -Ref <tag>), the SDK bundle and the tools the release names are downloaded
//            and checked, and the game is built the way the first build was (make_pc.ps1 swaps the bundle when it
//            is another one, and Ninja builds only what changed)
//   main     "Latest changes (main, untested)", from More: origin/main, built with the SDK that is installed; refused
//            when main changes the SDK's patches (a prebuilt SDK comes with a release only)
//
// The window drives this (LauncherApp), and so does the check harness (dev/wizard-check: update).
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

    /// Writes the channel into the install's record and the pointer beside the exe (when that names this install).
    public static void Write(string installRoot, string exeDirectory, string channel)
    {
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

/// An update that has been prepared (what it needs is downloaded and checked): the build to run, and the folder's
/// lock, held until the build is over.
sealed class UpdateSession : IDisposable
{
    readonly InstallLock folderLock;
    public BuildRunOptions Options { get; }
    public UpdateTarget Target { get; }
    /// Folders of tools this update replaced, to remove once the build worked.
    public IReadOnlyList<string> OldToolFolders { get; }
    /// Something the game is made of changed (files outside the launcher and the documentation, a new SDK bundle or
    /// tool): only then are the shaders prepared again after the build.
    public bool GameChanged { get; }

    internal UpdateSession(InstallLock folderLock, BuildRunOptions options, UpdateTarget target, IReadOnlyList<string> oldToolFolders, bool gameChanged)
    {
        this.folderLock = folderLock;
        Options = options;
        Target = target;
        OldToolFolders = oldToolFolders;
        GameChanged = gameChanged;
    }

    public void Dispose() => folderLock.Dispose();
}

static class UpdateEngine
{
    /// Takes the folder's lock and gets ready: for a release, the tools and the SDK bundle it names are downloaded
    /// and checked (what is already there is not fetched again); for main, the SDK's patches are compared. The build
    /// itself is `build-game.ps1 -Ref ...` with the options in the session.
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
                await MoveToAsync(layout, target, log);
                return new UpdateSession(folderLock, new BuildRunOptions(layout.LauncherDir, layout.LogsDir, [], layout.GitDir, IsUpdate: true), target, [], mainChanged);
            }

            var release = target.Release!;
            var source = target.Source!;
            var oldFolders = new List<string>();
            var installedAny = false;
            progress.Report("Checking the tools");
            foreach (var name in ReleaseInfo.RequiredTools)
            {
                var tool = release.Tools[name];
                if (ToolInstaller.IsInstalled(layout.Tools, name, tool.Sha256)) { log($"{name}: already unpacked"); continue; }
                var oldFolder = ToolInstaller.FolderOf(layout.Tools, name);
                progress.Report($"Downloading {name}");
                var file = Path.Combine(layout.Downloads, Path.GetFileName(new Uri(tool.Url).AbsolutePath));
                await Verified.FetchAsync(name, file, tool.Size, tool.Sha256,
                    (dest, p, token) => FetchTool(source, tool, dest, p, token), new Progress<long>(_ => { }), log, ct);
                progress.Report($"Unpacking {name}");
                var folder = await ToolInstaller.InstallAsync(layout.Tools, name, tool.Version, file, tool.Sha256, log, new Progress<(int, int)>(), ct);
                installedAny = true;
                Retry.Io(() => File.Delete(file), ct: ct);
                if (oldFolder != null && !SetupEngine.SamePath(oldFolder, folder)) oldFolders.Add(oldFolder);
            }

            var bundle = release.SdkBundle!;
            var zip = Path.Combine(layout.Downloads, bundle.Asset);
            progress.Report("Downloading the SDK");
            await Verified.FetchAsync("the SDK bundle", zip, bundle.Size, bundle.Sha256,
                (dest, p, token) => source.FetchAssetAsync(bundle.Asset, dest, p, token), new Progress<long>(_ => { }), log, ct);
            string[] arguments = ["-Toolchain", "mingw", "-ToolsDir", layout.Tools, "-SdkBundle", zip, "-SdkBundleSha256", bundle.Sha256];
            var gameChanged = installedAny || BundleInUse(layout) != bundle.Sha256.ToLowerInvariant() || await FilesChangedAsync(layout, target);
            progress.Report("Moving to the release");
            await MoveToAsync(layout, target, log);
            return new UpdateSession(folderLock, new BuildRunOptions(layout.LauncherDir, layout.LogsDir, arguments, layout.GitDir, IsUpdate: true), target, oldFolders, gameChanged);
        }
        catch
        {
            folderLock.Dispose();
            throw;
        }
    }

    /// Moves the checkout to the target commit (already fetched by UpdateResolver): forward, a fast-forward that carries
    /// what codegen rewrote across (an autostash, with an identity given for the stash commit: MinGit has none set);
    /// otherwise (back to a release from main) a checkout, which discards only those rewrites.
    static async Task MoveToAsync(InstallLayout layout, UpdateTarget target, Action<string> log)
    {
        var head = (await Proc.RunAsync("git", ["-C", layout.Repo, "rev-parse", "HEAD"], TimeSpan.FromSeconds(30))).Output.Trim();
        if (head == target.Commit) { log($"already at {target.Label}"); return; }
        var ancestor = (await Proc.RunAsync("git", ["-C", layout.Repo, "merge-base", "--is-ancestor", head, target.Commit], TimeSpan.FromSeconds(30))).ExitCode == 0;
        ProcResult moved;
        if (ancestor)
        {
            moved = await Proc.RunAsync("git", ["-c", "user.name=mcla", "-c", "user.email=mcla@localhost", "-C", layout.Repo, "merge", "--ff-only", "--autostash", target.Commit], TimeSpan.FromSeconds(120));
        }
        else
        {
            log($"{head[..7]} is not an ancestor of {target.Commit[..7]}: checking it out");
            moved = await Proc.RunAsync("git", ["-c", "advice.detachedHead=false", "-C", layout.Repo, "checkout", "--force", "--detach", target.Commit], TimeSpan.FromSeconds(120));
        }
        if (moved.ExitCode != 0)
            throw new SetupException("Couldn't move the install to the new version: " + (moved.Error + moved.Output).Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 0), moved.Error + moved.Output);
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

    /// After an update's build that did not finish: if the checkout is already at the update's commit, the SDK bundle and
    /// the tools of the update are what a plain Rebuild must use (make_pc.ps1 keeps the options only when a build
    /// finishes, and the old bundle is not the new checkout's SDK).
    public static void KeepOptionsAfterFailure(InstallLayout layout, UpdateSession session)
    {
        var target = session.Target;
        if (target.IsMain || target.Release?.SdkBundle is not { } bundle) return;
        var head = Proc.RunAsync("git", ["-C", layout.Repo, "rev-parse", "HEAD"], TimeSpan.FromSeconds(30)).GetAwaiter().GetResult().Output.Trim();
        if (head != target.Commit) return;
        var json = System.Text.Json.JsonSerializer.Serialize(new Dictionary<string, string>
        {
            ["toolchain"] = "mingw",
            ["toolsDir"] = layout.Tools,
            ["sdkBundle"] = Path.Combine(layout.Downloads, bundle.Asset),
            ["sdkBundleSha256"] = bundle.Sha256,
            ["sdkDir"] = Path.Combine(layout.Repo, "third_party", "rexglue-sdk-bin"),
        }, new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
        Retry.Io(() => File.WriteAllText(Path.Combine(layout.LauncherDir, "build-options.json"), json + "\r\n", new System.Text.UTF8Encoding(true)));
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

    /// After a build that worked: the record says which release the install is at, and what the update replaced
    /// (older tools, older SDK bundles) is removed.
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
            };
            updated.Save(layout.RecordFile);
            InstallChannel.SavePointer(updated, exeDirectory);
        }
        foreach (var old in session.OldToolFolders)
            if (Directory.GetParent(Path.GetFullPath(old))?.FullName.TrimEnd('\\') is { } parent
                && string.Equals(parent, Path.GetFullPath(layout.Tools).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                SafeFolder.Delete(old);
        if (!target.IsMain && target.Release?.SdkBundle is { } bundle && Directory.Exists(layout.Downloads))
            foreach (var zip in Directory.EnumerateFiles(layout.Downloads, "rexglue-sdk-*.zip"))
                if (!string.Equals(Path.GetFileName(zip), bundle.Asset, StringComparison.OrdinalIgnoreCase))
                    Retry.Io(() => File.Delete(zip));
    }
}

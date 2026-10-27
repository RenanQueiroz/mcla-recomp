// The setup: from a game and a release to a built game with its shaders prepared, step by step, resumable.
// No UI in here: the wizard's page and the check harness (launcher/dev/wizard-check) both drive this class
// and read its Steps.
//
//   Game     a) copy the game out of the disc image (or folder) into game-incoming\, each file hashed as it is
//               written; or, for "use it where it is", read it once to check it and copy only default.xex
//   Tools    b) download llvm-mingw, CMake, Ninja, MinGit and the SDK bundle in parallel, each checked against
//               release.json's SHA-256; unpack the tools
//   Source   c) git clone of the fork at the release tag; git rev-parse <tag> must be release.json's commit;
//               the game is moved into the checkout
//   Translate d) rexglue codegen   )  one run of build-game.ps1
//   Build    e) compile the game   )  (-Toolchain mingw -ToolsDir -SdkBundle)
//   Shaders  f) mcla.exe --mcla_prepare_shaders
//
// Game and Tools run at the same time; Source waits for Tools (it needs MinGit), the build for both.
//
// One setup at a time in an install folder (InstallLock); the programs it starts end with the wizard (ChildJob);
// every step checks what is really on disk before it skips, so a run that was cut short anywhere can be run again.
using System.Collections.Immutable;
using System.Diagnostics;
using System.Text;

namespace MclaLauncher.Setup;

enum StepId { Game, Tools, Source, Translate, Build, Shaders }

enum StepState { Waiting, Running, Done, Skipped, Failed, Cancelled }

sealed record StepStatus(StepId Id, string Title, StepState State, double? Fraction, string Text, string? Remaining)
{
    public bool Finished => State is StepState.Done or StepState.Skipped;
}

/// What the player chose.
sealed record SetupChoices(string InstallRoot, string GamePath, bool UseInPlace, string GameArguments = "");

/// The things the setup needs from outside, apart from the choices.
sealed record SetupContext(InstallLayout Layout, ReleaseSource Source, ReleaseInfo Release, SetupChoices Choices, string ExeDirectory);

/// A game already installed by an earlier setup: where it came from, and whether it stays where it was.
sealed record AdoptedGame(string Path, bool InPlace);

sealed class SetupEngine
{
    static readonly (StepId Id, string Title)[] Titles =
    [
        (StepId.Game, "Copying your game"),
        (StepId.Tools, "Downloading the build tools"),
        (StepId.Source, "Getting the source"),
        (StepId.Translate, "Translating the game's code"),
        (StepId.Build, "Building the game"),
        (StepId.Shaders, "Preparing shaders"),
    ];

    readonly SetupContext ctx;
    readonly object gate = new();
    readonly object saveGate = new();
    readonly Dictionary<StepId, StepStatus> steps;
    readonly Dictionary<StepId, List<string>> logs = Titles.ToDictionary(t => t.Id, _ => new List<string>());
    readonly Stopwatch clock = Stopwatch.StartNew();
    readonly Dictionary<StepId, Stopwatch> stepClocks = [];
    StreamWriter? logFile;
    DateTime lastNotify = DateTime.MinValue;
    BuildRun? build;
    ShaderPrepRun? shaders;
    InstallRecord record;
    string? bundleZip;
    string? failure;
    string? failureDetail;
    bool failureIsNetwork;

    public SetupEngine(SetupContext context)
    {
        ctx = context;
        steps = Titles.ToDictionary(t => t.Id, t => new StepStatus(t.Id, t.Title, StepState.Waiting, null, "", null));
        if (ctx.Choices.UseInPlace) steps[StepId.Game] = steps[StepId.Game] with { Title = "Checking your game" };
        record = InstallRecord.Load(ctx.Layout.RecordFile) is { } existing && SamePath(existing.InstallRoot, ctx.Layout.Root)
            ? existing with { InstallRoot = ctx.Layout.Root }
            : new InstallRecord { InstallRoot = ctx.Layout.Root };
    }

    /// The game of a folder that was set up before: its files are in the checkout (or, for "where it is", the folder the
    /// record names) and pass the check, so the disc image is not needed again. Null when there is no such game.
    public static AdoptedGame? FindInstalledGame(InstallLayout layout, GameFingerprint fingerprint)
    {
        var existing = InstallRecord.Load(layout.RecordFile);
        if (existing == null || !existing.StepsDone.Contains("Game") || !SamePath(existing.InstallRoot, layout.Root)) return null;
        var inPlace = existing.GameMode == "in-place";
        var wanted = inPlace ? new GameFingerprint(fingerprint.Title, fingerprint.TitleId, fingerprint.XexVersion, [fingerprint.Xex]) : fingerprint;
        if (!GameCopier.IsComplete(layout.GameDir, wanted)) return null;
        try
        {
            // default.xex is hashed again: it is what the build was made from.
            if (!Verified.Is(Path.Combine(layout.GameDir, GameFingerprint.XexName), fingerprint.Xex.Size, fingerprint.Xex.Sha256)) return null;
            if (inPlace)
            {
                if (string.IsNullOrEmpty(existing.GameDataRoot) || !Directory.Exists(existing.GameDataRoot)) return null;
                var result = GameChecker.Inspect(existing.GameDataRoot, fingerprint, out var source);
                source?.Dispose();
                return result.Ok ? new AdoptedGame(existing.GameDataRoot, true) : null;
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return null; }
        return new AdoptedGame(existing.GameSource ?? layout.GameDir, false);
    }

    /// False when the record beside the launcher's exe could not be written (the exe's folder is read-only).
    public bool PointerSaved { get; private set; } = true;

    /// Raised on any thread when something changed (at most a few times a second).
    public event Action? Changed;

    public IReadOnlyList<StepStatus> Steps
    {
        get { lock (gate) return Titles.Select(t => steps[t.Id]).ToList(); }
    }

    public ImmutableList<string> LogOf(StepId id) { lock (gate) return [.. logs[id]]; }
    public string? LogFilePath { get; private set; }
    /// The plain line that says what went wrong, and the detail for the log, after a failed run.
    public string? Failure { get { lock (gate) return failure; } }
    public string? FailureDetail { get { lock (gate) return failureDetail; } }
    /// The failure was a download that did not get through (a NetworkFailure: the page then offers the files downloaded by hand).
    public bool FailureIsNetwork { get { lock (gate) return failureIsNetwork; } }
    public InstallRecord Record => record;

    // ---------------------------------------------------------------------------------------------

    void Set(StepId id, Func<StepStatus, StepStatus> change, bool force = false)
    {
        bool notify;
        lock (gate)
        {
            var before = steps[id];
            var after = change(before);
            steps[id] = after;
            if (before.State != StepState.Running && after.State == StepState.Running) stepClocks[id] = Stopwatch.StartNew();
            var now = DateTime.UtcNow;
            notify = force || before.State != after.State || (now - lastNotify).TotalMilliseconds > 200;
            if (notify) lastNotify = now;
        }
        if (notify) Changed?.Invoke();
    }

    void Log(StepId id, string line)
    {
        lock (gate)
        {
            var list = logs[id];
            if (list.Count >= 3000) list.RemoveRange(0, 1000);
            list.Add(line);
            try { logFile?.WriteLine($"[{id}] {line}"); } catch (ObjectDisposedException) { }
        }
    }

    void Progress(StepId id, double? fraction, string text) =>
        Set(id, s => s with { Fraction = fraction, Text = text, Remaining = RemainingFor(id, fraction) });

    // Called with the lock held (from Set).
    string? RemainingFor(StepId id, double? fraction) =>
        fraction is double f && f > 0.02 && f < 1 && stepClocks.TryGetValue(id, out var stepClock) && stepClock.Elapsed.TotalSeconds > 8
            ? Eta(stepClock.Elapsed, f)
            : null;

    StepState StateOf(StepId id)
    {
        lock (gate) return steps[id].State;
    }

    /// "about 3 minutes left", from the share done and the time it took.
    public static string Eta(TimeSpan elapsed, double fraction)
    {
        var left = TimeSpan.FromSeconds(elapsed.TotalSeconds * (1 - fraction) / fraction);
        if (left.TotalSeconds < 15) return "a few seconds left";
        if (left.TotalSeconds < 90) return $"about {Math.Round(left.TotalSeconds / 10) * 10:0} seconds left";
        if (left.TotalMinutes < 90) return $"about {Math.Round(left.TotalMinutes):0} minutes left";
        return $"about {left.TotalHours:0.#} hours left";
    }

    // ---------------------------------------------------------------------------------------------

    /// Runs the whole setup. Returns normally when the game is built and ready; throws OperationCanceledException when
    /// cancelled and SetupException (also in Failure) when a step failed; steps that were done are kept for a retry.
    public async Task RunAsync(CancellationToken ct)
    {
        lock (gate) { failure = null; failureDetail = null; failureIsNetwork = false; }
        ChildJob.Enabled = true;
        InstallLock installLock;
        try { installLock = InstallLock.Acquire(ctx.Layout.Root); }
        catch (SetupException ex)
        {
            lock (gate) { failure = ex.Plain; failureDetail = ex.Detail; }
            foreach (var t in Titles) Set(t.Id, s => s with { State = StepState.Cancelled, Text = "Not started", Remaining = null }, force: true);
            Changed?.Invoke();
            throw;
        }
        using (installLock) await RunLockedAsync(ct);
    }

    async Task RunLockedAsync(CancellationToken ct)
    {
        var layout = ctx.Layout;
        Directory.CreateDirectory(layout.LogsDir);
        LogFilePath = Path.Combine(layout.LogsDir, $"setup-{DateTime.Now:yyyyMMdd-HHmmss}.log");
        logFile = new StreamWriter(LogFilePath, append: false, new UTF8Encoding(false)) { AutoFlush = true };
        foreach (var old in new DirectoryInfo(layout.LogsDir).GetFiles("setup-*.log").OrderByDescending(f => f.Name).Skip(10)) old.Delete();
        lock (gate) { failure = null; failureDetail = null; }
        foreach (var t in Titles) Set(t.Id, s => s with { State = StepState.Waiting, Fraction = null, Text = "", Remaining = null }, force: true);
        Log(StepId.Game, $"setup of {ctx.Release.Tag} ({ctx.Release.Commit[..8]}) into {layout.Root}; release from {ctx.Source.Describe}");

        try
        {
            Prepare();
            var fingerprint = ctx.Release.Game!.ToFingerprint();

            // The game's copy and the tools' download run side by side; a failure of one stops the other.
            using var group = CancellationTokenSource.CreateLinkedTokenSource(ct);
            var token = group.Token;
            async Task Chain(Func<Task> body)
            {
                try { await body(); }
                catch (Exception ex) when (ex is not OperationCanceledException) { group.Cancel(); throw; }
            }
            var gameChain = Chain(() => RunStep(StepId.Game, () => GameStepAsync(fingerprint, token), token));
            var toolsChain = Chain(async () =>
            {
                await RunStep(StepId.Tools, () => ToolsStepAsync(token), token);
                await RunStep(StepId.Source, () => SourceStepAsync(token), token);
            });
            await AllAsync(gameChain, toolsChain);

            FinishGame(fingerprint);
            await RunStep(StepId.Translate, () => BuildStepsAsync(ct), ct, alsoFinishes: [StepId.Build]);
            await RunStep(StepId.Shaders, () => ShadersStepAsync(ct), ct);
            Finish();
        }
        catch (OperationCanceledException)
        {
            foreach (var t in Titles) Set(t.Id, s => s.State is StepState.Running or StepState.Waiting ? s with { State = StepState.Cancelled, Text = "Cancelled" } : s, force: true);
            Log(StepId.Game, "cancelled");
            throw;
        }
        catch (Exception ex)
        {
            var plain = ex is SetupException se ? se.Plain : $"The setup stopped: {ex.Message}";
            lock (gate) { failure = plain; failureDetail = (ex as SetupException)?.Detail ?? ex.ToString(); failureIsNetwork = ex is SetupException { InnerException: NetworkFailure }; }
            foreach (var t in Titles) Set(t.Id, s => s.State is StepState.Running or StepState.Waiting ? s with { State = StepState.Cancelled, Text = "Stopped", Remaining = null } : s, force: true);
            Log(StepId.Game, $"FAILED: {plain}");
            if (failureDetail != null) Log(StepId.Game, failureDetail);
            Changed?.Invoke();
            if (ex is SetupException) throw;
            throw new SetupException(plain, ex.ToString(), ex);
        }
        finally
        {
            lock (gate) { logFile?.Dispose(); logFile = null; }
        }
    }

    /// Waits for all of them and throws the first real failure, not the cancellation it caused in the others.
    static async Task AllAsync(params Task[] tasks)
    {
        try { await Task.WhenAll(tasks); }
        catch
        {
            var real = tasks.Where(t => t.IsFaulted).Select(t => t.Exception!.InnerExceptions[0]).FirstOrDefault(e => e is not OperationCanceledException);
            if (real != null) System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(real).Throw();
            throw;
        }
    }

    async Task RunStep(StepId id, Func<Task> body, CancellationToken ct, StepId[]? alsoFinishes = null)
    {
        Set(id, s => s with { State = StepState.Running, Text = "Starting…" }, force: true);
        try
        {
            await body();
            if (StateOf(id) == StepState.Running)
                Set(id, s => s with { State = StepState.Done, Fraction = 1, Remaining = null }, force: true);
            foreach (var other in alsoFinishes ?? [])
                if (StateOf(other) is StepState.Running or StepState.Waiting)
                    Set(other, s => s with { State = StepState.Done, Fraction = 1, Remaining = null }, force: true);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch
        {
            Set(id, s => s with { State = StepState.Failed, Remaining = null }, force: true);
            throw;
        }
    }

    // ---------------------------------------------------------------------------------------------

    void Prepare()
    {
        var layout = ctx.Layout;
        Directory.CreateDirectory(layout.Root);
        // A write test first: a folder that can't be written fails here, in words, and not in a tool.
        var probe = Path.Combine(layout.Root, ".mcla-write-test");
        try { File.WriteAllText(probe, "x"); File.Delete(probe); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            throw new SetupException($"Can't write into {layout.Root}: {ex.Message} Choose another folder.", ex.ToString());
        }
        // What is in the folder already counts as done: only what is still left to do needs room (the game's bytes not
        // yet copied, the tools until they are unpacked, the build until it is made), with the same margin as a fresh start.
        var release = ctx.Release;
        bool toolsDone = ReleaseInfo.RequiredTools.All(name => ToolInstaller.IsInstalled(layout.Tools, name, release.Tools[name].Sha256));
        bool built = BuiltAlready();
        var gameWillChange = record.GameSource != null
            && ((record.GameMode == "in-place") != ctx.Choices.UseInPlace || !SamePath(record.GameSource, ctx.Choices.GamePath));
        var forCount = gameWillChange ? record with { StepsDone = record.StepsDone.Where(s => s != "Game").ToList() } : record;
        var needed = PathBudget.RemainingBytes(layout, forCount, release.Game!.Files.Sum(f => f.Size), !ctx.Choices.UseInPlace, toolsDone, built);
        var have = PathBudget.FreeBytes(layout.Root);
        if (have < needed)
            throw new SetupException($"There is not enough room on that drive: the setup needs about {GameChecker.FormatSize(needed)} more and {GameChecker.FormatSize(have)} is free. Free some space or choose another drive.",
                                     $"needed {needed}, free {have}");
        var mode = ctx.Choices.UseInPlace ? "in-place" : "copy";
        var done = record.StepsDone.ToList();
        // Another release than the one this folder was made from: its checkout, build and shaders are the old ones.
        if (record.ReleaseCommit != null && record.ReleaseCommit != release.Commit)
        {
            Log(StepId.Game, $"this folder was set up from {record.ReleaseTag}; it is replaced by {release.Tag} (settings are kept)");
            done.RemoveAll(s => s is "Source" or "Build" or "Shaders");
        }
        // Another game or another way of keeping it than before: the game is looked at again.
        if (record.GameSource != null && (record.GameMode != mode || !SamePath(record.GameSource, ctx.Choices.GamePath)))
            done.Remove("Game");
        lock (gate)
        {
            record = record with
            {
                ReleaseTag = release.Tag,
                ReleaseCommit = release.Commit,
                GameMode = mode,
                GameSource = ctx.Choices.GamePath,
                GameDataRoot = ctx.Choices.UseInPlace ? ctx.Choices.GamePath : null,
                State = "setting-up",
                StepsDone = done,
                FinishedUtc = null,
            };
        }
        Save();
    }

    /// The same place, whatever the slashes and the case ("C:/x/" and "c:\X").
    internal static bool SamePath(string? a, string? b)
    {
        if (a == null || b == null) return false;
        try { return string.Equals(Path.GetFullPath(a.Trim()).TrimEnd('\\'), Path.GetFullPath(b.Trim()).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase); }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return string.Equals(a.Trim().TrimEnd('\\', '/'), b.Trim().TrimEnd('\\', '/'), StringComparison.OrdinalIgnoreCase);
        }
    }

    /// The record is written outside the lock the window's reads take (a locked file may make a save wait a moment),
    /// and always as it is by then, so that two steps finishing together cannot leave the older one on disk.
    void Save()
    {
        lock (saveGate)
        {
            InstallRecord latest;
            lock (gate) latest = record;
            latest.Save(ctx.Layout.RecordFile);
            // The pointer beside the exe: how the launcher finds the install next time. A folder that can't be
            // written (the exe is somewhere read-only) is not fatal here, but the Ready page says so: the wizard
            // starts again next time, and the install folder it is given opens at once as a finished setup.
            try { latest.Save(InstallRecord.PointerFile(ctx.ExeDirectory)); PointerSaved = true; }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { PointerSaved = false; }
        }
    }

    void MarkDone(string step)
    {
        lock (gate)
        {
            if (!record.StepsDone.Contains(step)) record = record with { StepsDone = [.. record.StepsDone, step] };
        }
        Save();
    }

    // a) ----------------------------------------------------------------------------------------

    async Task GameStepAsync(GameFingerprint fingerprint, CancellationToken ct)
    {
        var layout = ctx.Layout;
        if (GameCopier.IsComplete(layout.GameDir, Wanted(fingerprint)) && record.StepsDone.Contains("Game"))
        {
            Log(StepId.Game, "the game is in place already");
            Set(StepId.Game, s => s with { State = StepState.Skipped, Text = "Already done", Fraction = 1 }, force: true);
            return;
        }
        var check = GameChecker.Inspect(ctx.Choices.GamePath, fingerprint, out var source);
        if (!check.Ok || source == null)
            throw new SetupException($"{check.Headline}. {check.Detail}", $"verdict {check.Verdict}");
        using (source)
        {
            var progress = new Progress<CopyProgress>(p =>
                Progress(StepId.Game, p.Total == 0 ? null : (double)p.Done / p.Total,
                         $"{GameChecker.FormatSize(p.Done)} of {GameChecker.FormatSize(p.Total)}  ·  {p.File}"));
            if (ctx.Choices.UseInPlace)
            {
                Log(StepId.Game, $"using the game where it is ({ctx.Choices.GamePath}); checking its files");
                await GameCopier.VerifyAsync(source, fingerprint, progress, ct);
                // The build reads default.xex from game\ in the checkout; the rest stays where it is.
                await GameCopier.CopyAsync(source, Wanted(fingerprint), layout.Staging, progress, ct);
            }
            else
            {
                Log(StepId.Game, $"copying the game from {source.Describe}");
                await GameCopier.CopyAsync(source, fingerprint, layout.Staging, progress, ct);
            }
        }
        Progress(StepId.Game, 1, ctx.Choices.UseInPlace ? "Checked" : "Copied and checked");
        Log(StepId.Game, "the game's files are checked");
    }

    /// What the install keeps of the game: all of it, or default.xex only for "use it where it is".
    GameFingerprint Wanted(GameFingerprint fingerprint) =>
        ctx.Choices.UseInPlace ? new GameFingerprint(fingerprint.Title, fingerprint.TitleId, fingerprint.XexVersion, [fingerprint.Xex]) : fingerprint;

    void FinishGame(GameFingerprint fingerprint)
    {
        var layout = ctx.Layout;
        var wanted = Wanted(fingerprint);
        if (!GameCopier.IsComplete(layout.GameDir, wanted))
        {
            if (!Directory.Exists(layout.Staging)) throw new SetupException("The game's files are not where the setup left them.", layout.Staging);
            GameCopier.MoveIntoPlace(layout.Staging, layout.GameDir, wanted);
            Log(StepId.Source, "the game is in place in the checkout");
        }
        MarkDone("Game");
    }

    // b) ----------------------------------------------------------------------------------------

    async Task ToolsStepAsync(CancellationToken ct)
    {
        var layout = ctx.Layout;
        var release = ctx.Release;
        var all = ReleaseInfo.RequiredTools.Select(name => (Name: name, Tool: release.Tools[name])).ToList();
        var bundle = release.SdkBundle!;
        bundleZip = Path.Combine(layout.Downloads, bundle.Asset);
        long total = all.Sum(t => t.Tool.Size) + bundle.Size;
        var received = new long[all.Count + 1];
        // A tool that is unpacked from the right archive (its marker says so) is not fetched or unpacked again:
        // a retry after a failed clone or build needs no network for it.
        var unpackedAlready = all.Select(t => ToolInstaller.IsInstalled(layout.Tools, t.Name, t.Tool.Sha256)).ToArray();
        for (int i = 0; i < all.Count; i++)
            if (unpackedAlready[i])
            {
                received[i] = all[i].Tool.Size;
                Log(StepId.Tools, $"{all[i].Name}: already unpacked");
            }
        void Report()
        {
            long sum = received.Sum();
            Progress(StepId.Tools, total == 0 ? null : Math.Min(1.0, (double)sum / total),
                     $"{GameChecker.FormatSize(sum)} of {GameChecker.FormatSize(total)}");
        }
        Progress(StepId.Tools, 0, "Downloading…");

        var downloads = new List<Task>();
        for (int i = 0; i < all.Count; i++)
        {
            if (unpackedAlready[i]) continue;
            int index = i;
            var (name, tool) = all[i];
            var file = Path.Combine(layout.Downloads, Path.GetFileName(new Uri(tool.Url).AbsolutePath));
            downloads.Add(Task.Run(() => Verified.FetchAsync(name, file, tool.Size, tool.Sha256,
                (dest, progress, token) => FetchTool(tool, dest, progress, token),
                new Progress<long>(n => { received[index] = n; Report(); }), line => Log(StepId.Tools, line), ct), ct));
        }
        downloads.Add(Task.Run(() => Verified.FetchAsync("the SDK bundle", bundleZip, bundle.Size, bundle.Sha256,
            (dest, progress, token) => ctx.Source.FetchAssetAsync(bundle.Asset, dest, progress, token),
            new Progress<long>(n => { received[^1] = n; Report(); }), line => Log(StepId.Tools, line), ct), ct));
        await Task.WhenAll(downloads);
        Progress(StepId.Tools, 1, "Unpacking…");

        int done = 0;
        for (int i = 0; i < all.Count; i++)
        {
            var (name, tool) = all[i];
            if (unpackedAlready[i]) { done++; continue; }
            var file = Path.Combine(layout.Downloads, Path.GetFileName(new Uri(tool.Url).AbsolutePath));
            CheckDepth(file, layout.Tools);
            await ToolInstaller.InstallAsync(layout.Tools, name, tool.Version, file, tool.Sha256, line => Log(StepId.Tools, line),
                new Progress<(int Done, int Total)>(p => Progress(StepId.Tools, null, $"Unpacking {name}…")), ct);
            done++;
        }
        CheckDepth(bundleZip, Path.Combine(layout.Repo, "third_party", "rexglue-sdk-bin"));
        // The tools' zips are not needed again.
        foreach (var (_, tool) in all)
            try { File.Delete(Path.Combine(layout.Downloads, Path.GetFileName(new Uri(tool.Url).AbsolutePath))); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        // (what is unpacked stays recorded in tools\.mcla-tools.txt, with the archive's SHA-256)
        Progress(StepId.Tools, 1, "Done");
        MarkDone("Tools");
    }

    Task FetchTool(ReleaseTool tool, string destination, IProgress<long> progress, CancellationToken ct)
    {
        // With a local release source, a tool archive of the same name beside release.json is used instead of the download.
        if (ctx.Source.LocalFolder is { } folder)
        {
            var name = Path.GetFileName(new Uri(tool.Url).AbsolutePath);
            foreach (var candidate in new[] { Path.Combine(folder, name), Path.Combine(folder, "tools", name) })
                if (File.Exists(candidate)) return ReleaseSource.DownloadAsync(candidate, destination, progress, ct, allowLocalFile: true);
        }
        if (!tool.Url.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
            throw new SetupException("The release names a download that isn't https; it was not used.", tool.Url);
        return ReleaseSource.DownloadAsync(tool.Url, destination, progress, ct);
    }

    /// The longest path an archive would make below `where`, from the install folder: refuses before unpacking
    /// what Windows' programs could not open.
    void CheckDepth(string zipFile, string where)
    {
        int longest;
        using (var zip = System.IO.Compression.ZipFile.OpenRead(zipFile))
            longest = zip.Entries.Max(e => e.FullName.Length);
        var relativeToRoot = Path.GetRelativePath(ctx.Layout.Root, where).Length + 1 + longest;
        var total = ctx.Layout.Root.TrimEnd('\\').Length + 1 + relativeToRoot;
        if (total > PathBudget.MaxPath)
            throw new SetupException($"The folder {ctx.Layout.Root} is too deep for the files in {Path.GetFileName(zipFile)} ({total} characters needed, Windows' programs stop at {PathBudget.MaxPath}). Choose a shorter folder, for example C:\\MCLA.",
                                     $"{zipFile}: longest entry {longest}");
    }

    // c) ----------------------------------------------------------------------------------------

    async Task SourceStepAsync(CancellationToken ct)
    {
        var layout = ctx.Layout;
        var release = ctx.Release;
        var git = layout.GitDir ?? throw new SetupException("MinGit isn't unpacked; the tools step did not finish.");
        var gitExe = Path.Combine(git, "git.exe");

        async Task<string?> Head()
        {
            var lines = new List<string>();
            var code = await ChildProcess.RunAsync(gitExe, ["-C", layout.Repo, "rev-parse", "HEAD"], null, git, lines.Add, ct);
            return code == 0 ? lines.FirstOrDefault()?.Trim() : null;
        }

        // Every tracked file of the working tree is there (git ls-files --deleted lists none). A clone that was killed
        // while it wrote the files has the right HEAD and a partial tree: that is not "there already".
        async Task<bool> TreeIsWhole()
        {
            var lines = new List<string>();
            var code = await ChildProcess.RunAsync(gitExe, ["-C", layout.Repo, "ls-files", "--deleted"], null, git, lines.Add, ct);
            return code == 0 && lines.Count == 0;
        }

        if (record.StepsDone.Contains("Source") && Directory.Exists(Path.Combine(layout.Repo, ".git")) && await Head() == release.Commit
            && File.Exists(layout.BuildScript) && await TreeIsWhole())
        {
            Log(StepId.Source, "the checkout is there already");
            Set(StepId.Source, s => s with { State = StepState.Skipped, Text = "Already done", Fraction = 1 }, force: true);
            await VerifyCheckout(gitExe, git, ct);
            return;
        }
        UnmarkDone("Source", "Build", "Shaders");

        if (Directory.Exists(layout.Repo))
        {
            // The player's settings in it are put aside first and come back after the build (RestoreSettings).
            KeepSettings();
            // An unfinished or older checkout: removed, after the game in it is put aside (it is the player's 6 GB).
            if (Directory.Exists(layout.GameDir) && !Directory.Exists(layout.Staging))
            {
                Retry.Io(() => Directory.Move(layout.GameDir, layout.Staging));
                Log(StepId.Source, "the game is put aside while the checkout is made again");
            }
            Log(StepId.Source, $"removing {layout.Repo} (not a complete checkout of {release.Tag})");
            SafeFolder.Delete(layout.Repo);
        }

        Progress(StepId.Source, 0, "Cloning the fork…");
        Log(StepId.Source, $"git clone --branch {release.Tag} {ctx.Source.GitUrl}");
        var ok = await ChildProcess.RunAsync(gitExe,
            ["-c", "core.longpaths=true", "-c", "advice.detachedHead=false", "clone", "--progress", "--branch", release.Tag, ctx.Source.GitUrl, layout.Repo],
            layout.Root, git, line =>
            {
                Log(StepId.Source, line);
                if (System.Text.RegularExpressions.Regex.Match(line, @"(Receiving objects|Resolving deltas|Updating files):\s+(\d+)%") is { Success: true } m)
                {
                    var part = m.Groups[1].Value switch { "Receiving objects" => 0.0, "Resolving deltas" => 0.6, _ => 0.8 };
                    var span = m.Groups[1].Value == "Receiving objects" ? 0.6 : 0.2;
                    Progress(StepId.Source, part + span * int.Parse(m.Groups[2].Value) / 100.0, "Cloning the fork…");
                }
            }, ct);
        if (ok != 0)
            throw new SetupException($"Couldn't get the source for {release.Tag} from {ctx.Source.GitUrl}. Check the internet connection and try again.", $"git clone exited with {ok}");
        await ChildProcess.RunAsync(gitExe, ["-C", layout.Repo, "config", "core.longpaths", "true"], null, git, _ => { }, ct);
        await VerifyCheckout(gitExe, git, ct);
        if (!await TreeIsWhole())
            throw new SetupException("The checkout is not complete after the clone. Try again.", "git ls-files --deleted lists files");
        MarkDone("Source");
    }

    /// The player's game settings (mcla.toml next to mcla.exe) and the launcher's settings.json of the checkout that is
    /// about to be replaced, copied to kept\ (never over what is already there from an earlier, cut-short run).
    void KeepSettings()
    {
        var layout = ctx.Layout;
        foreach (var (from, name) in SettingsFiles())
        {
            var to = Path.Combine(layout.Kept, name);
            if (!File.Exists(from) || File.Exists(to)) continue;
            Directory.CreateDirectory(layout.Kept);
            Retry.Io(() => File.Copy(from, to, overwrite: false));
            Log(StepId.Source, $"{name} is kept while the checkout is replaced");
        }
    }

    /// Puts the player's settings back into the new checkout, over the new files, and removes what it restored from kept\ (and kept\
    /// itself when nothing else is in it: an update keeps the player's edits of the recompiler's files there, in kept\codegen-edits).
    void RestoreSettings() => RestoreKept(ctx.Layout, SettingsFiles(), name => Log(StepId.Shaders, $"{name} is back"));

    internal static void RestoreKept(InstallLayout layout, IEnumerable<(string Path, string Name)> files, Action<string> restored)
    {
        if (!Directory.Exists(layout.Kept)) return;
        foreach (var (to, name) in files)
        {
            var from = Path.Combine(layout.Kept, name);
            if (!File.Exists(from)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            Retry.Io(() => File.Copy(from, to, overwrite: true));
            Retry.Io(() => File.Delete(from));
            restored(name);
        }
        if (!Directory.EnumerateFileSystemEntries(layout.Kept).Any()) SafeFolder.Delete(layout.Kept);
    }

    IEnumerable<(string Path, string Name)> SettingsFiles() =>
    [
        (Path.Combine(ctx.Layout.BuildDir, "mcla.toml"), "mcla.toml"),
        (Path.Combine(ctx.Layout.LauncherDir, "settings.json"), "settings.json"),
    ];

    void UnmarkDone(params string[] steps)
    {
        bool changed;
        lock (gate)
        {
            changed = record.StepsDone.Any(steps.Contains);
            if (changed) record = record with { StepsDone = record.StepsDone.Where(s => !steps.Contains(s)).ToList() };
        }
        if (changed) Save();
    }

    bool BuiltAlready()
    {
        var layout = ctx.Layout;
        var file = Path.Combine(layout.LauncherDir, "built-commit.txt");
        try { return File.Exists(layout.GameExe) && File.Exists(file) && File.ReadAllText(file).Trim() == ctx.Release.Commit; }
        catch (IOException) { return false; }
    }

    /// git rev-parse <tag> is the release's commit, HEAD is it, and the shader pack is the release's: what is built is
    /// what the release says, even if the tag was moved or the repository altered.
    async Task VerifyCheckout(string gitExe, string git, CancellationToken ct)
    {
        var layout = ctx.Layout;
        var release = ctx.Release;
        async Task<string> Rev(string rev)
        {
            var lines = new List<string>();
            var code = await ChildProcess.RunAsync(gitExe, ["-C", layout.Repo, "rev-parse", "--verify", rev], null, git, lines.Add, ct);
            return code == 0 ? lines.FirstOrDefault()?.Trim() ?? "" : "";
        }
        var tagCommit = await Rev($"{release.Tag}^{{commit}}");
        if (tagCommit != release.Commit)
            throw new SetupException($"The source's tag {release.Tag} points at {(tagCommit.Length >= 8 ? tagCommit[..8] : "nothing")}, not at the commit {release.Commit[..8]} the release names. It was not built: the repository doesn't match the release.",
                                     $"git rev-parse {release.Tag}^{{commit}} = '{tagCommit}', release.json commit = {release.Commit}");
        var head = await Rev("HEAD");
        if (head != release.Commit)
            throw new SetupException($"The checkout is at {(head.Length >= 8 ? head[..8] : "nothing")}, not at the release's commit {release.Commit[..8]}.", $"HEAD = '{head}'");
        Log(StepId.Source, $"{release.Tag} is {release.Commit[..8]}, as the release says");
        if (release.ShaderPack is { } pack)
        {
            var file = Path.Combine(layout.Repo, pack.Path.Replace('/', '\\'));
            if (!await Verified.IsAsync(file, pack.Size, pack.Sha256, ct))
                throw new SetupException("The shader pack in the source isn't the one the release names; it was not used.", file);
            Log(StepId.Source, "the shader pack matches the release");
        }
        Progress(StepId.Source, 1, $"{release.Tag} ({release.Commit[..8]})");
    }

    // d, e) -------------------------------------------------------------------------------------

    async Task BuildStepsAsync(CancellationToken ct)
    {
        var layout = ctx.Layout;
        if (BuiltAlready())
        {
            Log(StepId.Translate, "the game is built already");
            Set(StepId.Translate, s => s with { State = StepState.Skipped, Text = "Already done", Fraction = 1 }, force: true);
            Set(StepId.Build, s => s with { State = StepState.Skipped, Text = "Already done", Fraction = 1 }, force: true);
            return;
        }
        bundleZip ??= Path.Combine(layout.Downloads, ctx.Release.SdkBundle!.Asset);
        string[] arguments = ["-Toolchain", "mingw", "-ToolsDir", layout.Tools, "-SdkBundle", bundleZip, "-SdkBundleSha256", ctx.Release.SdkBundle!.Sha256];
        var options = new BuildRunOptions(layout.LauncherDir, layout.LogsDir, arguments, layout.GitDir);
        Log(StepId.Translate, $"build-game.ps1 {string.Join(' ', arguments)}");
        var run = new BuildRun(BuildKind.Build, line => Log(StepId.Build, line), options);
        build = run;
        using var registration = ct.Register(run.Stop);
        Set(StepId.Build, s => s with { State = StepState.Waiting, Text = "Waiting for the translation" }, force: true);
        Progress(StepId.Translate, null, "Starting…");

        var since = run.StartedUtc;
        using var poll = new PeriodicTimer(TimeSpan.FromSeconds(1));
        var finished = run.Completion;
        while (!finished.IsCompleted)
        {
            var activity = Activity.Read(since, Path.Combine(layout.Repo, "out"));
            if (activity != null)
            {
                if (activity.Step.StartsWith("Recompiling", StringComparison.Ordinal))
                {
                    if (StateOf(StepId.Translate) != StepState.Running) Set(StepId.Translate, s => s with { State = StepState.Running }, force: true);
                    Progress(StepId.Translate, null, "Translating…");
                }
                else if (activity.Step.StartsWith("Configuring", StringComparison.Ordinal) || activity.Step.StartsWith("Compiling", StringComparison.Ordinal))
                {
                    if (StateOf(StepId.Translate) == StepState.Running) Set(StepId.Translate, s => s with { State = StepState.Done, Fraction = 1, Remaining = null, Text = "Translated" }, force: true);
                    if (StateOf(StepId.Build) != StepState.Running) Set(StepId.Build, s => s with { State = StepState.Running }, force: true);
                    Progress(StepId.Build, activity.Percent is double pct ? pct / 100 : null, activity.Detail.Length > 0 ? activity.Detail : activity.Step);
                }
            }
            try { await Task.WhenAny(finished, Task.Delay(1000, ct)); } catch (OperationCanceledException) { }
            ct.ThrowIfCancellationRequested();
        }
        var exit = await finished;
        ct.ThrowIfCancellationRequested();
        if (exit != 0 || !File.Exists(layout.GameExe))
            throw new SetupException("Building the game failed. The details show where; Copy the log, and send it with a report.",
                                     $"build-game.ps1 exited with {exit}; log {run.LogFile}");
        Set(StepId.Translate, s => s.State == StepState.Done ? s : s with { State = StepState.Done, Fraction = 1, Remaining = null, Text = "Translated" }, force: true);
        Progress(StepId.Build, 1, "Built");
        MarkDone("Build");
    }

    // f) ----------------------------------------------------------------------------------------

    async Task ShadersStepAsync(CancellationToken ct)
    {
        var layout = ctx.Layout;
        RestoreSettings();
        WriteInitialSettings();
        if (ctx.Choices.UseInPlace) WriteGameDataRoot();
        else RemoveGameDataRoot();
        if (!File.Exists(Path.Combine(layout.Repo, "shaderpack", "545407F8.shaderpack")) || !File.Exists(layout.GameExe))
        {
            Set(StepId.Shaders, s => s with { State = StepState.Skipped, Text = "This version has no shader pack" }, force: true);
            return;
        }
        Progress(StepId.Shaders, null, "Starting…");
        var run = new ShaderPrepRun(layout.GameExe, ctx.Choices.GameArguments, p =>
        {
            Progress(StepId.Shaders, p.Percent / 100, p.Describe());
            Log(StepId.Shaders, p.Describe());
        }, layout.LogsDir, reason: null);
        shaders = run;
        using var registration = ct.Register(run.Stop);
        var exit = await run.Completion;
        ct.ThrowIfCancellationRequested();
        if (exit == 0)
        {
            Progress(StepId.Shaders, 1, run.PipelinesCreated > 0 ? $"{run.PipelinesCreated:N0} pipelines compiled" : "Done");
        }
        else
        {
            // Not worth stopping for: the game compiles shaders as it meets them, and More > Prepare shaders tries again.
            var why = ShaderPrepRun.Describe(exit, run.ErrorMessage);
            Log(StepId.Shaders, $"shaders were not prepared: {why}");
            Set(StepId.Shaders, s => s with { State = StepState.Skipped, Fraction = 1, Text = $"Skipped: {why}. The game still plays; More > Prepare shaders tries again." }, force: true);
        }
        MarkDone("Shaders");
    }

    /// settings.json of the new install: nothing, unless the development override gives the game extra options
    /// (the check harness's way to keep a test game away from the real saves and shader cache).
    void WriteInitialSettings()
    {
        if (string.IsNullOrWhiteSpace(ctx.Choices.GameArguments)) return;
        var file = Path.Combine(ctx.Layout.LauncherDir, "settings.json");
        if (File.Exists(file)) return;
        var json = System.Text.Json.JsonSerializer.Serialize(new Dictionary<string, object> { ["ExtraArguments"] = ctx.Choices.GameArguments, ["SettingsInGameFile"] = true },
                                                             new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
        File.WriteAllText(file, json, new UTF8Encoding(false));
        Log(StepId.Shaders, $"development: the game's extra options are set in settings.json ({ctx.Choices.GameArguments})");
    }

    /// "Use it where it is": mcla.toml next to mcla.exe names the game's folder.
    void WriteGameDataRoot()
    {
        var file = GameSettingsFile.PathIn(ctx.Layout.BuildDir);
        Directory.CreateDirectory(ctx.Layout.BuildDir);
        // No trailing backslash, except on a drive's root ("E:" alone would be that drive's current folder).
        var folder = ctx.Choices.GamePath.TrimEnd('\\');
        if (folder.Length == 2 && folder[1] == ':') folder += "\\";
        GameSettingsFile.Set(file, new Dictionary<string, object> { ["game_data_root"] = folder });
        Log(StepId.Shaders, $"game_data_root = {ctx.Choices.GamePath} in {file}");
    }

    /// A game kept inside the install: no key may send the game to a folder an earlier setup of this install named.
    void RemoveGameDataRoot()
    {
        var file = GameSettingsFile.PathIn(ctx.Layout.BuildDir);
        if (!File.Exists(file)) return;
        try
        {
            GameSettingsFile.Remove(file, "game_data_root");
        }
        catch (IOException ex)
        {
            throw new SetupException("mcla.toml can't be changed: " + ex.Message);
        }
    }

    void Finish()
    {
        lock (gate) record = record with { State = "ready", FinishedUtc = DateTime.UtcNow };
        Save();
        // downloads\ keeps the SDK bundle: build-options.json names it for every later build.
        Log(StepId.Shaders, "setup finished");
        Changed?.Invoke();
    }

    public void Cancel()
    {
        build?.Stop();
        shaders?.Stop();
    }
}

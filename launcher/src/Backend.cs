// Everything the launcher does besides drawing the window: where things are,
// settings, git, running build-game.ps1 and following its progress.
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace MclaLauncher;

/// How this launcher was started, decided once from where its exe is (see Paths):
///   Classic    the exe is in launcher\app\ of a fork checkout (build-game.ps1 is found above it): the user's own
///              install, and development builds. The launcher as it has always been.
///   Installed  the exe has an install.json beside it that names an install the setup wizard made.
///   Wizard     neither: the first run, or a setup that did not finish. The setup wizard.
enum LaunchMode { Classic, Installed, Wizard }

/// The launcher lives in launcher\ of the fork's checkout (published into launcher\app\); the repository is its
/// parent folder. An install made by the setup wizard has the same checkout (mcla-recomp\ in the install folder),
/// with the exe elsewhere: install.json beside the exe says where.
static class Paths
{
    public static readonly LaunchMode Mode;
    /// The install folder of an Installed launcher, or of an unfinished setup the Wizard resumes; else null.
    public static readonly string? InstallRoot;
    public static readonly string LauncherDir;
    public static readonly string Repo;
    public static readonly string RepoOut;
    public static readonly string BuildDir;
    public static readonly string GameExe;
    public static readonly string BuildScript;
    public static readonly string UpdateLauncherScript;
    public static readonly string BuiltCommitFile;
    public static readonly string SettingsFile;
    public static readonly string LogsDir;
    public static readonly string Icon = Path.Combine(AppContext.BaseDirectory, "launcher.ico");
    // Written by build-launcher.ps1: the last commit that changed the launcher
    // this exe was built from. Absent in a dev build.
    public static readonly string SourceCommitFile = Path.Combine(AppContext.BaseDirectory, "source-commit.txt");
    public const string RepoUrl = "https://github.com/RenanQueiroz/mcla-recomp";
    public const string IssuesUrl = "https://github.com/RenanQueiroz/mcla-recomp/issues";
    public const string UpstreamUrl = "https://github.com/holdmysocks/mcla-recomp";
    /// MinGit's cmd folder in an install made by the wizard (git.exe is not on that PC's PATH); else null. Looked for at
    /// every use: an update may replace the folder it was in.
    public static string? GitDir => Mode == LaunchMode.Installed ? new Setup.InstallLayout(InstallRoot!).GitDir : null;
    /// What to start for "git": MinGit's git.exe from the install (Installed mode), else the first git.exe on this
    /// process's PATH (CreateProcess looks a bare name up in the PATH of the process that starts it, not in the
    /// environment it gives the child), else Git for Windows' usual place, else just "git".
    public static string GitExe => FindGit(GitDir, Environment.GetEnvironmentVariable("PATH"));

    static Paths()
    {
        (Mode, InstallRoot, LauncherDir) = Detect(AppContext.BaseDirectory);
        Repo = Path.GetDirectoryName(LauncherDir)!;
        RepoOut = Path.Combine(Repo, "out");
        BuildDir = Path.Combine(RepoOut, @"build\win-amd64-release");
        GameExe = Path.Combine(BuildDir, "mcla.exe");
        BuildScript = Path.Combine(LauncherDir, "build-game.ps1");
        UpdateLauncherScript = Path.Combine(LauncherDir, "update-launcher.ps1");
        BuiltCommitFile = Path.Combine(LauncherDir, "built-commit.txt");
        SettingsFile = Path.Combine(LauncherDir, "settings.json");
        LogsDir = Path.Combine(LauncherDir, "logs");
    }

    internal static string FindGit(string? gitDir, string? path)
    {
        if (gitDir != null && File.Exists(Path.Combine(gitDir, "git.exe"))) return Path.Combine(gitDir, "git.exe");
        foreach (var entry in (path ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                var candidate = Path.Combine(entry.Trim().Trim('"'), "git.exe");
                if (File.Exists(candidate)) return candidate;
            }
            catch (ArgumentException) { }
        }
        foreach (var known in new[] { Environment.SpecialFolder.ProgramFiles, Environment.SpecialFolder.ProgramFilesX86 })
        {
            var candidate = Path.Combine(Environment.GetFolderPath(known), "Git", "cmd", "git.exe");
            if (File.Exists(candidate)) return candidate;
        }
        return "git";
    }

    /// The rule, in order:
    ///  1. The first folder up from the exe that holds build-game.ps1 is a fork checkout's launcher\: Classic.
    ///     (Nothing below changes how this works; the wizard is never looked at.)
    ///  2. Otherwise install.json beside the exe, naming an install folder whose mcla-recomp\launcher\ has
    ///     build-game.ps1: Installed.
    ///  3. Otherwise the wizard (install.json beside the exe without a finished checkout: a setup to resume).
    internal static (LaunchMode Mode, string? InstallRoot, string LauncherDir) Detect(string exeDirectory)
    {
        for (var dir = new DirectoryInfo(exeDirectory); dir != null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "build-game.ps1")))
                return (LaunchMode.Classic, null, dir.FullName);
        var pointer = Setup.InstallRecord.Load(Setup.InstallRecord.PointerFile(exeDirectory));
        if (pointer is { InstallRoot.Length: > 0 })
        {
            var layout = new Setup.InstallLayout(pointer.InstallRoot);
            if (File.Exists(layout.BuildScript) && pointer.Ready) return (LaunchMode.Installed, layout.Root, layout.LauncherDir);
            return (LaunchMode.Wizard, layout.Root, layout.LauncherDir);
        }
        return (LaunchMode.Wizard, null, Path.GetFullPath(Path.Combine(exeDirectory, "..")));
    }

    /// The program started from the launcher gets MinGit's folder first on its PATH in an install that has one:
    /// git is not installed on that PC.
    public static void Prepare(ProcessStartInfo psi)
    {
        psi.Environment["GIT_TERMINAL_PROMPT"] = "0"; // never wait for a password nobody can type
        AddGitDir(psi);
    }

    /// MinGit's folder first on the PATH of a program the launcher starts (Installed mode only; nothing otherwise).
    public static void AddGitDir(ProcessStartInfo psi)
    {
        if (GitDir == null) return;
        var path = psi.Environment.TryGetValue("PATH", out var current) ? current : Environment.GetEnvironmentVariable("PATH");
        psi.Environment["PATH"] = GitDir + ";" + path;
    }
}

/// What the launcher passes to mcla.exe: only the options typed under
/// Advanced, *More mcla.exe options*. The game's settings are in mcla.toml
/// (GameSettings.cs), which the game reads at start, and an option on the
/// command line wins over the file. The SDK settings the launcher used to
/// add every start are now the game's own PC defaults (src/mcla_app.cpp), and
/// vsync is turned off by the game unless the player turns it on.
static class GameOptions
{
    public static string Arguments(LauncherSettings s) => LastOfEach(s.ExtraArguments.Trim());

    /// Every option once, the last one typed winning: the SDK's command line
    /// parser fails on a repeated option, and the game then ignores every
    /// option on the command line (the error goes only to stderr). --name and
    /// --no-name are one option; a separated "--name value" keeps its value.
    internal static string LastOfEach(string arguments)
    {
        var items = new List<(string? Key, string Text)>();
        var tokens = Tokenize(arguments);
        for (int i = 0; i < tokens.Count; ++i)
        {
            string text = tokens[i];
            string? key = OptionKey(text);
            if (key != null && !text.Contains('=') && i + 1 < tokens.Count && !tokens[i + 1].StartsWith('-'))
            {
                text += " " + tokens[++i];
            }
            if (key != null)
            {
                items.RemoveAll(item => item.Key == key);
            }
            items.Add((key, text));
        }
        return string.Join(' ', items.Select(item => item.Text));
    }

    /// The value an option has in these arguments (the last one given), or null when it isn't
    /// there: "--name=value", "--name value", "--name" (true) or "--no-name" (false).
    internal static string? Value(string arguments, string name)
    {
        string? value = null;
        var tokens = Tokenize(arguments);
        for (int i = 0; i < tokens.Count; ++i)
        {
            var token = tokens[i];
            if (OptionKey(token) != name) continue;
            int equals = token.IndexOf('=');
            if (equals >= 0) value = token[(equals + 1)..].Trim('"');
            else if (i + 1 < tokens.Count && !tokens[i + 1].StartsWith('-')) value = tokens[++i].Trim('"');
            else value = token.StartsWith("--no-") ? "false" : "true";
        }
        return value;
    }

    static string? OptionKey(string token)
    {
        if (!token.StartsWith("--"))
        {
            return null;
        }
        string name = token[2..];
        int equals = name.IndexOf('=');
        if (equals >= 0)
        {
            name = name[..equals];
        }
        name = name.Trim('"');
        if (name.StartsWith("no-"))
        {
            name = name[3..];
        }
        return name.Length > 0 ? name : null;
    }

    // Separated by white space; a double-quoted span, quotes kept, stays in its token.
    static List<string> Tokenize(string text)
    {
        var tokens = new List<string>();
        var current = new StringBuilder();
        bool quoted = false;
        foreach (char c in text)
        {
            if (c == '"')
            {
                quoted = !quoted;
            }
            if (char.IsWhiteSpace(c) && !quoted)
            {
                if (current.Length > 0)
                {
                    tokens.Add(current.ToString());
                    current.Clear();
                }
                continue;
            }
            current.Append(c);
        }
        if (current.Length > 0)
        {
            tokens.Add(current.ToString());
        }
        return tokens;
    }
}

sealed record LauncherSettings
{
    // The drop-downs of launchers before 2026-10-16, which passed the game's
    // settings on the command line: read only to move the choices that forced
    // a value into mcla.toml, once (SettingsMigration), then dropped, and no
    // longer written to settings.json.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public int? Display { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public int? Resolution { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public int? Fps { get; init; }
    // Which list the Fps index refers to: 0 is the first one (Game setting,
    // 30, 60, 120, Uncapped), 1 added 40, 45 and 50.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public int? FpsList { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public int? Intro { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public int? Prompts { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public int? AntiAliasing { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public int? GraphicsApi { get; init; }
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public int? HudRate { get; init; }
    // Set once those choices are in mcla.toml (or there were none).
    public bool SettingsInGameFile { get; init; }

    public string ExtraArguments { get; init; } = "";
    public bool CheckForUpdates { get; init; } = true;
    public bool CloseOnLaunch { get; init; } = true;
    public bool AutoPlay { get; init; }
    // The pack (its fingerprint) an automatic shader preparation last failed for, other than
    // "in use": not tried again before every Play until the pack changes or More > Prepare
    // shaders succeeds.
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingNull)] public string? ShaderPrepSkipPack { get; init; }

    /// The launcher's settings; the first time, the old choices also go into
    /// the game's mcla.toml (SettingsMigration, which keeps a copy first). A
    /// settings.json that can't be read carries none over: its old choices are
    /// unknown, and defaults must not be written over the game's settings.
    public static LauncherSettings Load()
    {
        LauncherSettings settings;
        try
        {
            settings = File.Exists(Paths.SettingsFile)
                ? JsonSerializer.Deserialize(File.ReadAllText(Paths.SettingsFile), SettingsJson.Default.LauncherSettings) ?? new()
                : new();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            // Saving the defaults will replace it, so a copy is kept first.
            Debug.WriteLine($"settings: {ex.Message}; using defaults");
            try { File.Copy(Paths.SettingsFile, Paths.SettingsFile + ".unreadable", overwrite: true); }
            catch (Exception copy) when (copy is IOException or UnauthorizedAccessException) { }
            return SettingsMigration.Retire(new LauncherSettings());
        }
        return SettingsMigration.Run(settings, Paths.BuildDir, GameSettings.GameIsRunning);
    }

    public void Save()
    {
        // Write then rename, so a crash mid-write can't leave half a file.
        var temp = $"{Paths.SettingsFile}.{Environment.ProcessId}.tmp";
        try
        {
            using (var stream = new FileStream(temp, FileMode.Create, FileAccess.Write, FileShare.None))
            {
                stream.Write(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(this, SettingsJson.Default.LauncherSettings)));
                stream.Flush(flushToDisk: true);
            }
            File.Move(temp, Paths.SettingsFile, overwrite: true);
        }
        catch
        {
            try { File.Delete(temp); } catch (IOException) { }
            throw;
        }
    }
}

[JsonSourceGenerationOptions(WriteIndented = true)]
[JsonSerializable(typeof(LauncherSettings))]
partial class SettingsJson : JsonSerializerContext;

sealed record ProcResult(int ExitCode, string Output, string Error);

static class Proc
{
    /// Runs a program without a window and returns its output; kills it on timeout.
    public static async Task<ProcResult> RunAsync(string file, IEnumerable<string> args, TimeSpan timeout)
    {
        if (file == "git") file = Paths.GitExe;
        var psi = new ProcessStartInfo(file)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var arg in args) psi.ArgumentList.Add(arg);
        Paths.Prepare(psi);

        using var process = Process.Start(psi) ?? throw new InvalidOperationException($"could not start {file}");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        using var cts = new CancellationTokenSource(timeout);
        try
        {
            await process.WaitForExitAsync(cts.Token);
        }
        catch (OperationCanceledException)
        {
            try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            throw new TimeoutException($"{Path.GetFileName(file)} {string.Join(' ', psi.ArgumentList)} timed out");
        }
        return new(process.ExitCode, (await output).TrimEnd(), (await error).TrimEnd());
    }
}

sealed record Commit(string Hash, string Date, string Subject);

sealed record RepoStatus(
    string Head,
    IReadOnlyList<Commit> Recent,      // newest first, starting at HEAD
    IReadOnlyList<Commit> Incoming,    // the fork's commits not checked out yet
    bool PatchesChanged,               // an update would change the SDK patches
    bool GameBuilt,                    // mcla.exe exists
    bool BuildCurrent,                 // the last finished build was of HEAD
    int UpstreamAhead,                 // holdmysocks/mcla-recomp commits not in the fork
    bool LauncherOutdated,             // the checkout has a newer launcher than this exe
    string? FetchError)
{
    /// When FetchError came from the network: the report for "Copy details" (what was tried, the addresses and their timings, the proxy).
    public string? FetchDetail { get; init; }
    /// Installed mode: where the install would move to (a release, or main); null in a clone or when not looked at.
    public Setup.UpdateTarget? Target { get; init; }
    /// The release (a v… tag) the checked-out commit is, shown as the version in place of the commit; null when HEAD is no release.
    public string? ReleaseTag { get; init; }
    public bool UpdateAvailable => Incoming.Count > 0 || Target is { SwitchBack: true };
}

static class Repo
{
    const string LogFormat = "--format=%h%x1f%cs%x1f%s";

    static Task<ProcResult> Git(params string[] args) =>
        Proc.RunAsync("git", ["-C", Paths.Repo, .. args], TimeSpan.FromSeconds(60));

    /// The release tag (v…) that points at HEAD, the highest if several; null when HEAD is not a release or git can't say.
    static async Task<string?> ReleaseTagAtHeadAsync()
    {
        var tags = await Git("tag", "--points-at", "HEAD", "--list", "v*", "--sort=-v:refname");
        return tags.ExitCode == 0
            ? tags.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).FirstOrDefault()
            : null;
    }

    static List<Commit> ParseLog(string text) =>
        text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(line => line.Split('\x1f'))
            .Where(parts => parts.Length == 3)
            .Select(parts => new Commit(parts[0], parts[1], parts[2]))
            .ToList();

    /// An install made by the setup wizard: what it would move to is a release (release.json on GitHub) or, when the
    /// player asked for it, origin/main; the commits between are the "What's new".
    static async Task<RepoStatus> GetInstalledStatusAsync(bool fetch)
    {
        string? fetchError = null;
        string? fetchDetail = null;
        Setup.UpdateTarget? target = null;
        // An update that began and was not built (the launcher closed, a crash): the options are made to name its SDK bundle.
        try { await Setup.UpdateEngine.RepairPendingAsync(new Setup.InstallLayout(Paths.InstallRoot!), AppContext.BaseDirectory); }
        catch (Exception) { }   // best effort: the status below is what the window needs
        try
        {
            target = await Setup.UpdateResolver.ResolveAsync(Paths.Repo, Setup.InstallChannel.Read(Paths.InstallRoot!), fetch, CancellationToken.None);
        }
        catch (Setup.SetupException ex) { fetchError = ex.Plain; fetchDetail = ex.Detail; }
        catch (TimeoutException) { fetchError = "GitHub didn't answer in time."; }

        var head = (await Git("rev-parse", "HEAD")).Output.Trim();
        var recent = ParseLog((await Git("log", "-8", LogFormat)).Output);
        var incoming = new List<Commit>();
        var patchesChanged = false;
        if (target != null)
        {
            incoming = ParseLog((await Git("log", LogFormat, $"HEAD..{target.Commit}")).Output);
            if (incoming.Count > 0 || target.SwitchBack)
                patchesChanged = (await Git("diff", "--name-only", "HEAD", target.Commit, "--", "patches/*.patch")).Output.Trim().Length > 0;
        }

        // The release names the commit its launcher was built from, as source-commit.txt (beside the exe) says for this one.
        var launcherOutdated = false;
        if (target?.Release?.Launcher is { SourceCommit: { Length: > 0 } wanted } && File.Exists(Paths.SourceCommitFile))
            launcherOutdated = File.ReadAllText(Paths.SourceCommitFile).Trim() != wanted;

        var builtCommit = File.Exists(Paths.BuiltCommitFile) ? File.ReadAllText(Paths.BuiltCommitFile).Trim() : null;
        var buildCurrent = builtCommit is null || builtCommit == head
            || (await Git("diff", "--name-only", builtCommit, head, "--", ".", ":(exclude)launcher", ":(exclude)*.md"))
                   is { ExitCode: 0, Output: var changed } && changed.Trim().Length == 0;
        // The release this install is: the tag on HEAD (the setup clones a release tag, an update fetches it), else what the
        // install record says it was set up or updated to, when that is this commit.
        var releaseTag = await ReleaseTagAtHeadAsync();
        if (releaseTag == null
            && Setup.InstallRecord.Load(Path.Combine(Paths.InstallRoot!, Setup.InstallLayout.RecordName)) is { } record
            && record.ReleaseTag is { Length: > 0 } recordTag && recordTag.StartsWith('v')
            && record.ReleaseCommit is { Length: > 0 } recordCommit && head.StartsWith(recordCommit, StringComparison.OrdinalIgnoreCase))
            releaseTag = recordTag;
        return new RepoStatus(head, recent, incoming, patchesChanged, File.Exists(Paths.GameExe), buildCurrent, 0, launcherOutdated, fetchError)
        {
            Target = target,
            FetchDetail = fetchDetail,
            ReleaseTag = releaseTag,
        };
    }

    public static async Task<RepoStatus> GetStatusAsync(bool fetch)
    {
        if (!Directory.Exists(Path.Combine(Paths.Repo, ".git")))
            throw new DirectoryNotFoundException($"There is no mcla-recomp checkout at {Paths.Repo}.");
        if (Paths.Mode == LaunchMode.Installed) return await GetInstalledStatusAsync(fetch);

        string? fetchError = null;
        var hasUpstream = (await Git("remote")).Output.Split('\n').Any(r => r.Trim() == "upstream");
        if (fetch)
        {
            try
            {
                var result = await Proc.RunAsync("git", ["-C", Paths.Repo, "fetch", "--quiet", "origin"], TimeSpan.FromSeconds(45));
                if (result.ExitCode != 0)
                    fetchError = result.Error.Split('\n').FirstOrDefault(l => l.Trim().Length > 0)?.Trim() ?? $"git fetch failed ({result.ExitCode})";
            }
            catch (TimeoutException)
            {
                fetchError = "GitHub didn't answer in time.";
            }
            // Only to say how far behind upstream the fork is; the fork is
            // brought up to date by hand (FORK.md), so a failure here is ignored.
            if (fetchError == null && hasUpstream)
            {
                try { await Proc.RunAsync("git", ["-C", Paths.Repo, "fetch", "--quiet", "upstream"], TimeSpan.FromSeconds(45)); }
                catch (TimeoutException) { }
            }
        }

        var head = (await Git("rev-parse", "HEAD")).Output.Trim();
        var recent = ParseLog((await Git("log", "-8", LogFormat)).Output);

        var incoming = new List<Commit>();
        var patchesChanged = false;
        var upstreamAhead = 0;
        if ((await Git("rev-parse", "--verify", "--quiet", "@{u}")).ExitCode == 0)
        {
            incoming = ParseLog((await Git("log", LogFormat, "HEAD..@{u}")).Output);
            if (incoming.Count > 0)
                patchesChanged = (await Git("diff", "--name-only", "HEAD...@{u}", "--", "patches/*.patch")).Output.Length > 0;
            if (hasUpstream && (await Git("rev-list", "--count", "@{u}..upstream/main")) is { ExitCode: 0 } ahead)
                int.TryParse(ahead.Output.Trim(), out upstreamAhead);
        }

        // An update that changed the launcher itself: build-launcher.ps1 records
        // the last commit that changed it, and the checkout now has a later one.
        var launcherOutdated = false;
        if (File.Exists(Paths.SourceCommitFile))
        {
            var builtFrom = File.ReadAllText(Paths.SourceCommitFile).Trim();
            var latest = (await Proc.RunAsync("git", ["-C", Paths.LauncherDir, "log", "-1", "--format=%H", "--", "src", "build-launcher.ps1"],
                                              TimeSpan.FromSeconds(60))).Output.Trim();
            launcherOutdated = latest.Length > 0 && builtFrom != latest;
        }

        // build-game.ps1 writes the commit it built once a build succeeds. No
        // record (a build made by hand with make_pc.ps1) counts as current, and
        // so does a build whose commit differs from HEAD only in the launcher
        // or in documentation, which the game build doesn't use.
        var builtCommit = File.Exists(Paths.BuiltCommitFile) ? File.ReadAllText(Paths.BuiltCommitFile).Trim() : null;
        var buildCurrent = builtCommit is null || builtCommit == head
            || (await Git("diff", "--name-only", builtCommit, head, "--", ".", ":(exclude)launcher", ":(exclude)*.md"))
                   is { ExitCode: 0, Output: var changed } && changed.Trim().Length == 0;
        return new RepoStatus(head, recent, incoming, patchesChanged,
            GameBuilt: File.Exists(Paths.GameExe),
            BuildCurrent: buildCurrent,
            upstreamAhead, launcherOutdated,
            fetchError)
        {
            // A checkout at a release (git fetch brings the tags of the commits it fetches).
            ReleaseTag = await ReleaseTagAtHeadAsync(),
        };
    }
}

enum BuildKind { Update, Build, ResetSdk }

/// Where and how build-game.ps1 runs when it isn't the launcher's own checkout (the setup wizard's first
/// build): the launcher folder to run in, and the options to pass (-Toolchain, -ToolsDir, -SdkBundle ...).
/// IsUpdate: the run is an update of an install made by the setup (the window says "Updating"); the checkout was moved to
/// its new commit before, so the script is run as a plain build.
sealed record BuildRunOptions(string LauncherDir, string LogsDir, IReadOnlyList<string> Arguments, string? GitDir = null, bool IsUpdate = false);

/// One run of build-game.ps1, its output streamed line by line and saved to launcher\logs\.
sealed class BuildRun
{
    const int KeepLogs = 15;

    readonly Process process;
    readonly StreamWriter log;
    readonly Action<string> onLine;

    public BuildKind Kind { get; }
    /// An update, whatever the script is asked to do.
    public bool IsUpdate => Kind == BuildKind.Update || isUpdate;
    readonly bool isUpdate;
    public DateTime StartedUtc { get; } = DateTime.UtcNow;
    public string LogFile { get; }
    public Task<int> Completion { get; }
    public bool StopRequested { get; private set; }

    public BuildRun(BuildKind kind, Action<string> onLine, BuildRunOptions? options = null)
    {
        Kind = kind;
        isUpdate = options?.IsUpdate == true;
        this.onLine = onLine;
        var launcherDir = options?.LauncherDir ?? Paths.LauncherDir;
        var logsDir = options?.LogsDir ?? Paths.LogsDir;
        Directory.CreateDirectory(logsDir);
        foreach (var old in new DirectoryInfo(logsDir).GetFiles("build-*.log").OrderByDescending(f => f.Name).Skip(KeepLogs - 1))
            old.Delete();
        LogFile = Path.Combine(logsDir, $"build-{DateTime.Now:yyyyMMdd-HHmmss}.log");
        log = new StreamWriter(LogFile, append: false, new UTF8Encoding(false)) { AutoFlush = true };

        var psi = new ProcessStartInfo("powershell.exe")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = launcherDir,
        };
        // -NonInteractive: if make_pc.ps1 finds a tool missing and asks to
        // install it, the build fails with that message instead of waiting
        // for an answer nobody can give.
        string[] args = ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", Path.Combine(launcherDir, "build-game.ps1")];
        foreach (var arg in args) psi.ArgumentList.Add(arg);
        if (kind == BuildKind.Update) psi.ArgumentList.Add("-Update");
        if (kind == BuildKind.ResetSdk) psi.ArgumentList.Add("-ResetSdk");
        foreach (var arg in options?.Arguments ?? []) psi.ArgumentList.Add(arg);
        Paths.Prepare(psi);
        // The install's own MinGit, when the build is the wizard's (the launcher's Paths do not know it yet).
        if (options?.GitDir is { } gitDir && Paths.GitDir == null)
            psi.Environment["PATH"] = gitDir + ";" + (psi.Environment.TryGetValue("PATH", out var p) ? p : Environment.GetEnvironmentVariable("PATH"));

        process = new Process { StartInfo = psi };
        process.OutputDataReceived += (_, e) => { if (e.Data != null) Emit(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data != null) Emit(e.Data); };
        process.Start();
        Setup.ChildJob.AssignIfEnabled(process);
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        Emit($"build-game.ps1 {string.Join(' ', psi.ArgumentList.Skip(args.Length))}".TrimEnd() + $"  ({DateTime.Now:yyyy-MM-dd HH:mm:ss})");

        Completion = Task.Run(() =>
        {
            process.WaitForExit(); // without a timeout this also waits for the output to drain
            lock (log) log.Dispose();
            return process.ExitCode;
        });
    }

    void Emit(string line)
    {
        lock (log)
        {
            try { log.WriteLine(line); } catch (ObjectDisposedException) { }
        }
        onLine(line);
    }

    public void Stop()
    {
        StopRequested = true;
        try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
    }
}

/// What the build is doing right now, read from the end of the newest log
/// that make_pc.ps1 and build.ps1 write under mcla-recomp\out\ (Windows
/// PowerShell writes them as UTF-16). Ninja's "[n/m]" lines give a percentage.
sealed record Activity(string Step, string Detail, double? Percent)
{
    static readonly Regex NinjaLine = new(@"^\[(\d+)/(\d+)\]\s*(.*)$");

    static readonly Dictionary<string, string> Steps = new(StringComparer.OrdinalIgnoreCase)
    {
        ["install-clang"] = "Downloading Clang",
        ["install-windows-sdk"] = "Installing the Windows SDK",
        ["sdk-clone"] = "Downloading the ReXGlue SDK",
        ["sdk-submodules"] = "Completing the ReXGlue SDK download",
        ["sdk-configure"] = "Configuring the SDK build",
        ["sdk-build"] = "Building the ReXGlue SDK",
        ["sdk-install"] = "Installing the SDK",
        ["extract"] = "Extracting the game from the disc image",
        ["codegen"] = "Recompiling the game's code",
        ["configure"] = "Configuring the game build",
        ["build"] = "Compiling the game",
    };

    public static Activity? Read(DateTime sinceUtc) => Read(sinceUtc, Paths.RepoOut);

    /// repoOut: the checkout's out\ folder, where make_pc.ps1 and build.ps1 write their logs.
    public static Activity? Read(DateTime sinceUtc, string repoOut)
    {
        try
        {
            var logs = new List<FileInfo>();
            if (Directory.Exists(repoOut)) logs.AddRange(new DirectoryInfo(repoOut).GetFiles("*.log"));
            var stepLogs = Path.Combine(repoOut, "logs");
            if (Directory.Exists(stepLogs)) logs.AddRange(new DirectoryInfo(stepLogs).GetFiles("*.log"));
            var newest = logs.Where(f => f.LastWriteTimeUtc >= sinceUtc.AddSeconds(-2))
                             .OrderByDescending(f => f.LastWriteTimeUtc)
                             .FirstOrDefault();
            if (newest == null) return null;

            var name = Path.GetFileNameWithoutExtension(newest.Name);
            var step = Steps.GetValueOrDefault(name, name);
            var line = LastLine(newest.FullName) ?? "";
            var match = NinjaLine.Match(line);
            if (match.Success && double.TryParse(match.Groups[2].Value, out var total) && total > 0)
                return new Activity(step, $"{match.Groups[1].Value} of {total}: {match.Groups[3].Value}", 100 * double.Parse(match.Groups[1].Value) / total);
            return new Activity(step, line, null);
        }
        catch (IOException)
        {
            return null; // a log being rotated or deleted; try again next tick
        }
    }

    static string? LastLine(string path)
    {
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        var bom = new byte[2];
        var utf16 = stream.Read(bom, 0, 2) == 2 && bom[0] == 0xFF && bom[1] == 0xFE;
        var start = Math.Max(utf16 ? 2 : 0, stream.Length - 8192);
        if (utf16 && start % 2 != 0) start++; // stay on a character boundary
        stream.Seek(start, SeekOrigin.Begin);
        var buffer = new byte[stream.Length - start];
        stream.ReadExactly(buffer);
        var text = (utf16 ? Encoding.Unicode : Encoding.UTF8).GetString(buffer);
        // curl redraws its progress with \r, so split on both.
        return text.Split('\r', '\n').LastOrDefault(l => l.Trim().Length > 0)?.Trim();
    }
}

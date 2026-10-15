// Everything the launcher does besides drawing the window: where things are,
// settings, git, running build-game.ps1 and following its progress.
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace MclaLauncher;

/// The launcher lives in launcher\ of the fork's checkout (published into
/// launcher\app\); the repository is its parent folder.
static class Paths
{
    public static readonly string LauncherDir = FindLauncherDir();
    public static readonly string Repo = Path.GetDirectoryName(LauncherDir)!;
    public static readonly string RepoOut = Path.Combine(Repo, "out");
    public static readonly string BuildDir = Path.Combine(RepoOut, @"build\win-amd64-release");
    public static readonly string GameExe = Path.Combine(BuildDir, "mcla.exe");
    public static readonly string BuildScript = Path.Combine(LauncherDir, "build-game.ps1");
    public static readonly string UpdateLauncherScript = Path.Combine(LauncherDir, "update-launcher.ps1");
    public static readonly string BuiltCommitFile = Path.Combine(LauncherDir, "built-commit.txt");
    public static readonly string SettingsFile = Path.Combine(LauncherDir, "settings.json");
    public static readonly string LogsDir = Path.Combine(LauncherDir, "logs");
    public static readonly string Icon = Path.Combine(AppContext.BaseDirectory, "launcher.ico");
    // Written by build-launcher.ps1: the last commit that changed the launcher
    // this exe was built from. Absent in a dev build.
    public static readonly string SourceCommitFile = Path.Combine(AppContext.BaseDirectory, "source-commit.txt");
    public const string RepoUrl = "https://github.com/RenanQueiroz/mcla-recomp";
    public const string UpstreamUrl = "https://github.com/holdmysocks/mcla-recomp";

    // The first folder up from the exe that holds build-game.ps1, so the same
    // exe works from launcher\app\ and from a dev build under launcher\src\bin\.
    static string FindLauncherDir()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "build-game.ps1")))
                return dir.FullName;
        return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, ".."));
    }
}

/// One choice in a game option drop-down: what it shows and the mcla.exe flag it adds.
sealed record Choice(string Label, string? Argument);

/// The game's command-line options (docs\pc-build-guide.md). The first choice
/// of each passes nothing, which leaves the value from the in-game pause menu
/// (saved in mcla.toml next to mcla.exe) or the game's default.
static class GameOptions
{
    const string Unset = "Game setting";
    public static readonly Choice[] Display = [new(Unset, null), new("Fullscreen", "--fullscreen=true"), new("Windowed", "--fullscreen=false")];
    // Every value is passed explicitly, defaults included: the in-game menu saves
    // what is in effect to mcla.toml, and a value left off the command line would
    // fall back to that file instead of the choice shown here.
    //
    // The recomp paces frames with its own limiter and means vsync to be off,
    // but turns it off only in mcla.exe's copy of the setting; the GPU plugin
    // keeps vsync on, and its emulated 60 Hz vblank (driven by a 1 ms sleep loop)
    // then caps the game at 60 and paces it unevenly at any target. On the
    // command line the setting reaches the plugin too.
    //
    // 40-50 are for G-Sync/FreeSync displays: a cap the game holds even in its
    // heaviest scenes gives constant frame times, which look smoother than a
    // higher rate that keeps dropping below its cap.
    public static readonly Choice[] Fps = [new(Unset, null),
        new("30 FPS", "--mcla_fps=30 --vsync=false"), new("40 FPS", "--mcla_fps=40 --vsync=false"),
        new("45 FPS", "--mcla_fps=45 --vsync=false"), new("50 FPS", "--mcla_fps=50 --vsync=false"),
        new("60 FPS", "--mcla_fps=60 --vsync=false"), new("120 FPS", "--mcla_fps=120 --vsync=false"),
        new("Uncapped", "--mcla_fps=0 --vsync=false")];
    // The SDK's draw resolution scale: the game renders at a multiple of the
    // console's 1280x720 instead of having that stretched to the screen. The
    // GPU cost grows with the square: 3× draws nine times the pixels.
    public static readonly Choice[] Resolution = [new("Native (720p)", "--resolution_scale=1"),
        new("2× (1440p)", "--resolution_scale=2"), new("3× (2160p, 4K)", "--resolution_scale=3")];
    public static readonly Choice[] Intro = [new(Unset, null), new("Normal", "--mcla_intro=normal"), new("Fast", "--mcla_intro=fast"), new("Skip (hides the title logo)", "--mcla_intro=skip")];
    public static readonly Choice[] Prompts = [new(Unset, null), new("Follow the controller", "--mcla_button_prompts=auto"), new("Xbox", "--mcla_button_prompts=xbox"), new("PlayStation", "--mcla_button_prompts=playstation")];
    // The game's 2× MSAA. Off, a 3× frame took 18 to 25% less time (with about
    // half the draws), for slightly more jagged thin edges.
    public static readonly Choice[] AntiAliasing = [new(Unset, null), new("On (2× MSAA)", "--mcla_msaa=true"), new("Off (about 20% faster at 3×)", "--mcla_msaa=false")];
    // The GPU plugin's backend. Left unset it tries Direct3D 12 first.
    public static readonly Choice[] GraphicsApi = [new(Unset, null), new("Direct3D 12", "--mcla_gpu_backend=d3d12"), new("Vulkan", "--mcla_gpu_backend=vulkan")];
    // The HUD's Flash movies (gauges, minimap, song ticker) take one step per
    // game update. At the console's 30 a second the ticker scrolls at its
    // original speed but the needles move in 30 Hz steps; at the frame rate the
    // gauges are smooth and the ticker runs faster above 30 FPS. Menus keep the
    // console's pace either way. Not in the game's menu, so no "Game setting".
    public static readonly Choice[] HudUpdates = [new("Match frame rate (smooth gauges)", "--mcla_ui_timing_hud=false"),
        new("Match console (30 per second)", "--mcla_ui_timing_hud=true")];

    // SDK settings that measured faster on MCLA (see README.md), always passed:
    // - uploaded guest memory stays valid across frames instead of being copied
    //   to the GPU again every frame (about 21% less GPU command thread time);
    // - vertex shaders of draws with clipping off run on the CPU, as in Xenia,
    //   so they no longer claim the whole EDRAM (about 11% faster at 3×);
    // - hot pages, as on the PS5: guest memory the game rewrites all the time
    //   is uploaded when drawn from instead of being write-protected again
    //   after every fault (about 9% shorter frames at native, uncapped).
    // A boolean given twice adds up instead of the last one winning, and any
    // other option given twice is a command-line error (CLI11 rejects repeats
    // by default), so a default is left out when the extra options set it.
    static readonly (string Name, string Argument)[] Tuning = [
        ("clear_memory_page_state", "--clear_memory_page_state=false"),
        ("execute_unclipped_draw_vs_on_cpu", "--execute_unclipped_draw_vs_on_cpu=true"),
        ("shared_memory_hot_page_faults", "--shared_memory_hot_page_faults=4"),
        ("shared_memory_hot_page_ms", "--shared_memory_hot_page_ms=10000")];

    public static string Labels(Choice[] choices, int index) => choices[Math.Clamp(index, 0, choices.Length - 1)].Label;

    public static string Arguments(LauncherSettings s)
    {
        string? Pick(Choice[] choices, int index) => choices[Math.Clamp(index, 0, choices.Length - 1)].Argument;
        var extra = s.ExtraArguments.Trim();
        var tuning = Tuning.Where(t => !extra.Contains(t.Name)).Select(t => t.Argument);
        // Left out when the extra options set them, like the defaults above.
        var antiAliasing = extra.Contains("mcla_msaa") ? null : Pick(AntiAliasing, s.AntiAliasing);
        var graphicsApi = extra.Contains("mcla_gpu_backend") ? null : Pick(GraphicsApi, s.GraphicsApi);
        var hudUpdates = extra.Contains("mcla_ui_timing_hud") ? null : Pick(HudUpdates, s.HudUpdates);
        var args = new[] { Pick(Display, s.Display), Pick(Resolution, s.Resolution), Pick(Fps, s.Fps),
                           Pick(Intro, s.Intro), Pick(Prompts, s.Prompts), antiAliasing, graphicsApi, hudUpdates }.Concat(tuning).Append(extra);
        return string.Join(" ", args.Where(a => !string.IsNullOrEmpty(a)));
    }
}

sealed record LauncherSettings
{
    public int Display { get; init; }
    public int Resolution { get; init; }
    public int Fps { get; init; }
    // Which GameOptions.Fps list the Fps index refers to: 0 is the first one
    // (Game setting, 30, 60, 120, Uncapped), 1 added 40, 45 and 50.
    public int FpsList { get; init; }
    public int Intro { get; init; }
    public int Prompts { get; init; }
    public int AntiAliasing { get; init; }
    public int GraphicsApi { get; init; }
    public int HudUpdates { get; init; }
    public string ExtraArguments { get; init; } = "";
    public bool CheckForUpdates { get; init; } = true;
    public bool CloseOnLaunch { get; init; } = true;
    public bool AutoPlay { get; init; }

    public static LauncherSettings Load()
    {
        try
        {
            if (File.Exists(Paths.SettingsFile))
                return Migrate(JsonSerializer.Deserialize(File.ReadAllText(Paths.SettingsFile), SettingsJson.Default.LauncherSettings) ?? new());
        }
        catch (Exception ex) when (ex is IOException or JsonException)
        {
            Debug.WriteLine($"settings: {ex.Message}; using defaults");
        }
        return Migrate(new());
    }

    // Where each choice of the first Fps list sits in the current one.
    static readonly int[] FpsFromList0 = [0, 1, 5, 6, 7];

    static LauncherSettings Migrate(LauncherSettings s) =>
        s.FpsList >= 1 ? s : s with { Fps = s.Fps >= 0 && s.Fps < FpsFromList0.Length ? FpsFromList0[s.Fps] : 0, FpsList = 1 };

    public void Save()
    {
        // Write then rename, so a crash mid-write can't leave half a file.
        var temp = Paths.SettingsFile + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(this, SettingsJson.Default.LauncherSettings));
        File.Move(temp, Paths.SettingsFile, overwrite: true);
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
        psi.Environment["GIT_TERMINAL_PROMPT"] = "0"; // never wait for a password nobody can type

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
    public bool UpdateAvailable => Incoming.Count > 0;
}

static class Repo
{
    const string LogFormat = "--format=%h%x1f%cs%x1f%s";

    static Task<ProcResult> Git(params string[] args) =>
        Proc.RunAsync("git", ["-C", Paths.Repo, .. args], TimeSpan.FromSeconds(60));

    static List<Commit> ParseLog(string text) =>
        text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(line => line.Split('\x1f'))
            .Where(parts => parts.Length == 3)
            .Select(parts => new Commit(parts[0], parts[1], parts[2]))
            .ToList();

    public static async Task<RepoStatus> GetStatusAsync(bool fetch)
    {
        if (!Directory.Exists(Path.Combine(Paths.Repo, ".git")))
            throw new DirectoryNotFoundException($"There is no mcla-recomp checkout at {Paths.Repo}.");

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
        // record (a build made by hand with make_pc.ps1) counts as current.
        var builtCommit = File.Exists(Paths.BuiltCommitFile) ? File.ReadAllText(Paths.BuiltCommitFile).Trim() : null;
        return new RepoStatus(head, recent, incoming, patchesChanged,
            GameBuilt: File.Exists(Paths.GameExe),
            BuildCurrent: builtCommit is null || builtCommit == head,
            upstreamAhead, launcherOutdated,
            fetchError);
    }
}

enum BuildKind { Update, Build, ResetSdk }

/// One run of build-game.ps1, its output streamed line by line and saved to launcher\logs\.
sealed class BuildRun
{
    const int KeepLogs = 15;

    readonly Process process;
    readonly StreamWriter log;
    readonly Action<string> onLine;

    public BuildKind Kind { get; }
    public DateTime StartedUtc { get; } = DateTime.UtcNow;
    public string LogFile { get; }
    public Task<int> Completion { get; }
    public bool StopRequested { get; private set; }

    public BuildRun(BuildKind kind, Action<string> onLine)
    {
        Kind = kind;
        this.onLine = onLine;
        Directory.CreateDirectory(Paths.LogsDir);
        foreach (var old in new DirectoryInfo(Paths.LogsDir).GetFiles("build-*.log").OrderByDescending(f => f.Name).Skip(KeepLogs - 1))
            old.Delete();
        LogFile = Path.Combine(Paths.LogsDir, $"build-{DateTime.Now:yyyyMMdd-HHmmss}.log");
        log = new StreamWriter(LogFile, append: false, new UTF8Encoding(false)) { AutoFlush = true };

        var psi = new ProcessStartInfo("powershell.exe")
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Paths.LauncherDir,
        };
        // -NonInteractive: if make_pc.ps1 finds a tool missing and asks to
        // install it, the build fails with that message instead of waiting
        // for an answer nobody can give.
        string[] args = ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", Paths.BuildScript];
        foreach (var arg in args) psi.ArgumentList.Add(arg);
        if (kind == BuildKind.Update) psi.ArgumentList.Add("-Update");
        if (kind == BuildKind.ResetSdk) psi.ArgumentList.Add("-ResetSdk");
        psi.Environment["GIT_TERMINAL_PROMPT"] = "0";

        process = new Process { StartInfo = psi };
        process.OutputDataReceived += (_, e) => { if (e.Data != null) Emit(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data != null) Emit(e.Data); };
        process.Start();
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
        ["sdk-clone"] = "Downloading the ReXGlue SDK",
        ["sdk-configure"] = "Configuring the SDK build",
        ["sdk-build"] = "Building the ReXGlue SDK",
        ["sdk-install"] = "Installing the SDK",
        ["extract"] = "Extracting the game from the disc image",
        ["codegen"] = "Recompiling the game's code",
        ["configure"] = "Configuring the game build",
        ["build"] = "Compiling the game",
    };

    public static Activity? Read(DateTime sinceUtc)
    {
        try
        {
            var logs = new List<FileInfo>();
            if (Directory.Exists(Paths.RepoOut)) logs.AddRange(new DirectoryInfo(Paths.RepoOut).GetFiles("*.log"));
            var stepLogs = Path.Combine(Paths.RepoOut, "logs");
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

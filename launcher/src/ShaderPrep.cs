// Preparing shaders: mcla.exe --mcla_prepare_shaders (src/shader_prepare.cpp) fills the game's
// shader storage from the shader pack in shaderpack\ (rebuilt from the player's own game files)
// and has the graphics driver compile every pipeline with the settings the game is played with,
// then quits. The launcher runs it after a build or an update, and from More, Prepare shaders
// (after a graphics driver update, which empties the driver's cache). Kept free of the
// launcher's UI so that the setup wizard can run it the same way.
using System.Diagnostics;
using System.Text;

namespace MclaLauncher;

/// Where a preparation run is: rebuilding the pack's shaders from the game, then creating the
/// stored pipelines.
sealed record ShaderPrepProgress(string Phase, int Done, int Total)
{
    public string Describe() => Phase switch
    {
        "rebuild" => Total > 0 ? $"Rebuilding shaders from your game: {Done:N0} of {Total:N0}" : "Reading your game's shaders",
        "preload" => Total > 0 ? $"Compiling pipelines: {Done:N0} of {Total:N0}" : "Compiling pipelines",
        _ => "Starting",
    };
    public double? Percent => Total > 0 ? 100.0 * Done / Total : null;
}

/// One run of mcla.exe --mcla_prepare_shaders, its standard output read line by line (the lines
/// starting with "SHADERPREP ") and saved to a log.
sealed class ShaderPrepRun
{
    const int KeepLogs = 10;
    static readonly TimeSpan Timeout = TimeSpan.FromMinutes(15);

    readonly Process process;
    readonly StreamWriter? log;
    readonly object logLock = new();

    public Task<int> Completion { get; }
    public string? LogFile { get; }
    /// The last line of the "done" report: rebuild ms, pipelines ms, private MB, GPU MB.
    public string? Summary { get; private set; }
    public string? ErrorMessage { get; private set; }
    public int PipelinesCreated { get; private set; }

    /// The pack the fork ships, if this checkout has one.
    public static string PackFile(string repo) => Path.Combine(repo, "shaderpack", "545407F8.shaderpack");

    /// gameExe: mcla.exe; options: the same options the game is started with (they decide the
    /// graphics API and the resolution scale, which the driver compiles for); logsDir: where to
    /// keep a log of the run, or null.
    public ShaderPrepRun(string gameExe, string options, Action<ShaderPrepProgress> onProgress, string? logsDir)
    {
        if (logsDir != null)
        {
            Directory.CreateDirectory(logsDir);
            foreach (var old in new DirectoryInfo(logsDir).GetFiles("prepare-shaders-*.log").OrderByDescending(f => f.Name).Skip(KeepLogs - 1))
                old.Delete();
            LogFile = Path.Combine(logsDir, $"prepare-shaders-{DateTime.Now:yyyyMMdd-HHmmss}.log");
            log = new StreamWriter(LogFile, append: false, new UTF8Encoding(false)) { AutoFlush = true };
        }
        // The player's options first; the preparation's own last, so that they win (the game
        // takes every option once: GameOptions.LastOfEach).
        var arguments = GameOptions.LastOfEach($"{options} --mcla_prepare_shaders=true");
        var psi = new ProcessStartInfo(gameExe, arguments)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(gameExe)!,
        };
        Write($"mcla.exe {arguments}  ({DateTime.Now:yyyy-MM-dd HH:mm:ss})");

        process = new Process { StartInfo = psi };
        process.OutputDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            Write(e.Data);
            if (Parse(e.Data) is { } progress) onProgress(progress);
        };
        process.ErrorDataReceived += (_, e) => { if (e.Data != null) Write(e.Data); };
        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        Completion = Task.Run(() =>
        {
            // A run that hangs (a crash dialog behind the hidden window) is ended: the storage is
            // only ever replaced whole, and the game's next start creates what is left.
            if (!process.WaitForExit(Timeout))
            {
                Write($"no exit after {Timeout.TotalMinutes:0} minutes; ending it");
                Stop();
            }
            process.WaitForExit();
            Write($"exit code {process.ExitCode}");
            lock (logLock) log?.Dispose();
            return process.ExitCode;
        });
    }

    ShaderPrepProgress? Parse(string line)
    {
        if (!line.StartsWith("SHADERPREP ")) return null;
        var parts = line["SHADERPREP ".Length..].Split(' ', 3);
        int Number(int i) => parts.Length > i && int.TryParse(parts[i], out var n) ? n : 0;
        switch (parts[0])
        {
            case "start": return new("rebuild", 0, Number(1));
            case "rebuild": return new("rebuild", Number(1), Number(2));
            case "rebuild-done": return new("preload", 0, 0);
            case "preload": return new("preload", Number(1), Number(2));
            case "preload-done":
                PipelinesCreated = Number(1);
                return new("preload", Number(2), Number(2));
            case "done":
                Summary = parts.Length > 1 ? string.Join(' ', parts[1..]) : "";
                return null;
            case "error":
                ErrorMessage = parts.Length > 2 ? parts[2] : null;
                return null;
            default: return null;
        }
    }

    void Write(string line)
    {
        lock (logLock)
        {
            try { log?.WriteLine(line); } catch (ObjectDisposedException) { }
        }
    }

    public void Stop()
    {
        try { process.Kill(entireProcessTree: true); }
        catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception) { }
    }

    /// What an exit code means, for a status line.
    public static string Describe(int exitCode, string? message) => exitCode switch
    {
        0 => "Shaders prepared",
        2 => "No shader pack in this version",
        3 => "The shader pack is damaged",
        4 => Sentence(message) ?? "Your game files don't match the shader pack",
        5 => "The shader pack is for another version of the game's graphics code",
        6 => Sentence(message) ?? "Couldn't write the shader storage",
        _ => $"The game stopped while preparing shaders (exit code {exitCode})",
    };

    static string? Sentence(string? message) =>
        string.IsNullOrEmpty(message) ? null : char.ToUpperInvariant(message[0]) + message[1..];
}

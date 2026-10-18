// Preparing shaders: mcla.exe --mcla_prepare_shaders (src/shader_prepare.cpp) adds what the
// game's shader storage lacks of the shader pack in shaderpack\ (rebuilt from the player's own
// game files) and has the graphics driver compile every stored pipeline with the settings the
// game is played with, then quits. The launcher runs it after a build or an update, before Play
// when the stamp says the driver, the settings or the storage changed (ShaderStamp.cs), and from
// More, Prepare shaders. Kept free of the launcher's UI so that the setup wizard can run it the
// same way.
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace MclaLauncher;

/// Why a preparation runs: after a build or an update, before Play (the stamp said so), or asked
/// for (More > Prepare shaders, which also tries a pack whose import was disabled again).
enum PrepareKind { AfterBuild, BeforePlay, Manual }

/// Where a preparation run is: rebuilding the pack's shaders from the game, then creating the
/// stored pipelines.
sealed record ShaderPrepProgress(string Phase, int Done, int Total)
{
    public string Describe() => Phase switch
    {
        "rebuild" => Total > 0 ? $"Rebuilding shaders from your game: {Done:N0} of {Total:N0}" : "Checking the shader storage",
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

    /// Exit codes (src/shader_prepare.cpp). In use is the only one worth trying again soon.
    public const int InUse = 7;

    readonly Process process;
    readonly StreamWriter? log;
    readonly object logLock = new();
    readonly JobObject? job;

    public Task<int> Completion { get; }
    public string? LogFile { get; }
    /// Why it runs, for the status line ("the graphics driver was updated"); null when asked for.
    public string? Reason { get; }
    /// The "done" report: rebuild ms, pipelines ms, private MB, GPU MB, process ms.
    public string? Summary { get; private set; }
    public string? ErrorMessage { get; private set; }
    public int PipelinesCreated { get; private set; }
    /// The pack's import was disabled after failed starts (More > Prepare shaders tries again).
    public bool ImportDisabled { get; private set; }

    /// The pack the fork ships, if this checkout has one.
    public static string PackFile(string repo) => Path.Combine(repo, "shaderpack", "545407F8.shaderpack");

    /// gameExe: mcla.exe; options: the same options the game is started with (they decide the
    /// graphics API and the resolution scale, which the driver compiles for); logsDir: where to
    /// keep a log of the run, or null; retry: also import a pack whose import was disabled after
    /// failed starts (the user asked for it).
    public ShaderPrepRun(string gameExe, string options, Action<ShaderPrepProgress> onProgress, string? logsDir,
                         string? reason = null, bool retry = false)
    {
        Reason = reason;
        string? gameLog = null;
        if (logsDir != null)
        {
            Directory.CreateDirectory(logsDir);
            foreach (var old in new DirectoryInfo(logsDir).GetFiles("prepare-shaders-2*.log").OrderByDescending(f => f.Name).Skip(KeepLogs - 1))
            {
                try { old.Delete(); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
            }
            LogFile = Path.Combine(logsDir, $"prepare-shaders-{DateTime.Now:yyyyMMdd-HHmmss-fff}.log");
            log = new StreamWriter(LogFile, append: false, new UTF8Encoding(false)) { AutoFlush = true };
            // The game's own log of the run, apart from the user's --log_file (which would
            // otherwise lose the last game session's log).
            gameLog = Path.Combine(logsDir, "prepare-shaders-game.log");
        }
        // The player's options first; the preparation's own last, so that they win (the game
        // takes every option once: GameOptions.LastOfEach).
        var own = " --mcla_prepare_shaders=true" + (retry ? " --mcla_prepare_shaders_retry=true" : "")
                + (gameLog != null ? $" --log_file=\"{gameLog}\"" : "");
        var arguments = GameOptions.LastOfEach(options + own);
        var psi = new ProcessStartInfo(gameExe, arguments)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(gameExe)!,
        };
        Write($"mcla.exe {arguments}  ({DateTime.Now:yyyy-MM-dd HH:mm:ss}){(reason != null ? $"; {reason}" : "")}");

        process = new Process { StartInfo = psi };
        process.OutputDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            Write(e.Data);
            if (Parse(e.Data) is { } progress) onProgress(progress);
        };
        process.ErrorDataReceived += (_, e) => { if (e.Data != null) Write(e.Data); };
        process.Start();
        // Ends with the launcher: a launcher that dies (killed, crashed, logged off) takes the
        // hidden mcla.exe with it.
        job = JobObject.KillOnClose(process);
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();
        Completion = Task.Run(() =>
        {
            try
            {
                // A run that hangs (a crash dialog behind the hidden window) is ended: the storage
                // is only ever replaced whole, and the game's next start undoes an unfinished import.
                if (!process.WaitForExit(Timeout))
                {
                    Write($"no exit after {Timeout.TotalMinutes:0} minutes; ending it");
                    Stop();
                }
                process.WaitForExit();
                Write($"exit code {process.ExitCode}");
                return process.ExitCode;
            }
            catch (Exception ex) when (ex is InvalidOperationException or Win32Exception)
            {
                Write($"lost the process: {ex.Message}");
                return -1;
            }
            finally
            {
                lock (logLock) log?.Dispose();
                job?.Dispose();
            }
        });
    }

    ShaderPrepProgress? Parse(string line)
    {
        if (!line.StartsWith("SHADERPREP ")) return null;
        var parts = line["SHADERPREP ".Length..].Split(' ');
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
            case "import-disabled":
                ImportDisabled = true;
                return null;
            case "done":
                Summary = string.Join(' ', parts[1..]);
                return null;
            case "error":
                ErrorMessage = parts.Length > 2 ? string.Join(' ', parts[2..]) : null;
                return null;
            default: return null;
        }
    }

    void Write(string line)
    {
        lock (logLock)
        {
            // A log that can't be written (a full disk) mustn't stop the run.
            try { log?.WriteLine(line); }
            catch (Exception ex) when (ex is ObjectDisposedException or IOException) { }
        }
    }

    public void Stop()
    {
        try { process.Kill(entireProcessTree: true); }
        catch (Exception ex) when (ex is InvalidOperationException or Win32Exception) { }
    }

    /// Whether a failure is worth trying again at the next Play: only "in use" is.
    public static bool IsTransient(int exitCode) => exitCode == InUse;

    /// What an exit code means, for a status line.
    public static string Describe(int exitCode, string? message) => exitCode switch
    {
        0 => "Shaders prepared",
        2 => "No shader pack in this version",
        3 => Sentence(message) ?? "The shader pack is damaged",
        4 => Sentence(message) ?? "Your game files don't match the shader pack",
        5 => "The shader pack is for another version of the game's graphics code",
        6 => Sentence(message) ?? "Couldn't read or write the shader storage",
        InUse => "The shader storage is in use by a running game; it's prepared at the next Play",
        _ => $"The game stopped while preparing shaders (exit code {exitCode})",
    };

    static string? Sentence(string? message) =>
        string.IsNullOrEmpty(message) ? null : char.ToUpperInvariant(message[0]) + message[1..];
}

/// A Windows job object that kills its process when the launcher's handle to it closes.
sealed class JobObject : IDisposable
{
    readonly IntPtr handle;

    JobObject(IntPtr handle) => this.handle = handle;

    public static JobObject? KillOnClose(Process process)
    {
        var handle = CreateJobObject(IntPtr.Zero, null);
        if (handle == IntPtr.Zero) return null;
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags = 0x2000;  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        var size = Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>();
        var buffer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(info, buffer, false);
            if (!SetInformationJobObject(handle, 9 /* JobObjectExtendedLimitInformation */, buffer, (uint)size) ||
                !AssignProcessToJobObject(handle, process.Handle))
            {
                CloseHandle(handle);
                return null;
            }
        }
        catch (Exception ex) when (ex is InvalidOperationException or Win32Exception)
        {
            CloseHandle(handle);
            return null;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
        return new JobObject(handle);
    }

    public void Dispose() => CloseHandle(handle);

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS
    {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
        public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObject(IntPtr attributes, string? name);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint length);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);
}

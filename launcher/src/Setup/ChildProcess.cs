// Running a program for the setup and reading what it prints line by line (progress lines that end with a
// carriage return, as git and curl write them, count as lines). No window, no prompts; killed with its
// children when the setup is cancelled.
using System.Diagnostics;
using System.Text;

namespace MclaLauncher.Setup;

static class ChildProcess
{
    /// Runs `file` and returns its exit code. `gitDir`: a folder put first on PATH (MinGit's cmd).
    public static async Task<int> RunAsync(string file, IEnumerable<string> args, string? workingDirectory, string? gitDir,
                                           Action<string> onLine, CancellationToken ct)
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
        if (workingDirectory != null) psi.WorkingDirectory = workingDirectory;
        foreach (var arg in args) psi.ArgumentList.Add(arg);
        psi.Environment["GIT_TERMINAL_PROMPT"] = "0";
        if (gitDir != null) psi.Environment["PATH"] = gitDir + ";" + (psi.Environment.TryGetValue("PATH", out var p) ? p : Environment.GetEnvironmentVariable("PATH"));

        using var process = Process.Start(psi) ?? throw new SetupException($"Couldn't start {Path.GetFileName(file)}.");
        ChildJob.AssignIfEnabled(process);
        using var registration = ct.Register(() =>
        {
            try { process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
        });
        var out1 = Pump(process.StandardOutput, onLine);
        var out2 = Pump(process.StandardError, onLine);
        await process.WaitForExitAsync(CancellationToken.None);
        await Task.WhenAll(out1, out2);
        ct.ThrowIfCancellationRequested();
        return process.ExitCode;
    }

    static async Task Pump(StreamReader reader, Action<string> onLine)
    {
        var line = new StringBuilder();
        var buffer = new char[4096];
        int read;
        while ((read = await reader.ReadAsync(buffer, 0, buffer.Length)) > 0)
        {
            for (int i = 0; i < read; i++)
            {
                char c = buffer[i];
                if (c == '\r' || c == '\n')
                {
                    if (line.Length > 0) { onLine(line.ToString()); line.Clear(); }
                }
                else line.Append(c);
            }
        }
        if (line.Length > 0) onLine(line.ToString());
    }
}

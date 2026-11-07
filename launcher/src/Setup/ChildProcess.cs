// Running a program for the setup and reading what it prints line by line (progress lines that end with a
// carriage return, as git and curl write them, count as lines). No window, no prompts; killed with its
// children when the setup is cancelled.
using System.Diagnostics;
using System.Text;

namespace MclaLauncher.Setup;

/// Git refuses a repository whose folder it can't show to be the user's ("detected dubious ownership", exit code 128): on a
/// drive that keeps no owner (exFAT, FAT32), in a folder owned by another account or by Administrators, or on a network
/// path. After the setup's clone, every git command in the checkout is then refused (issue #64). The checkouts git is run on
/// here are the ones the launcher made itself, so they are trusted: safe.directory=* for every git the launcher starts and the
/// programs those start (build-game.ps1 and make_pc.ps1 keep the same setting in their own environment variables). It is
/// given in the child's environment (git reads safe.directory from there and from the command line, never from the
/// repository), so nobody's git configuration is changed.
static class GitTrust
{
    public static void Apply(IDictionary<string, string?> environment)
    {
        // Settings already in GIT_CONFIG_COUNT stay: this one is added after them.
        var count = environment.TryGetValue("GIT_CONFIG_COUNT", out var text) && int.TryParse(text, out var n) && n >= 0 ? n : 0;
        environment[$"GIT_CONFIG_KEY_{count}"] = "safe.directory";
        environment[$"GIT_CONFIG_VALUE_{count}"] = "*";
        environment["GIT_CONFIG_COUNT"] = (count + 1).ToString();
    }
}

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
        GitTrust.Apply(psi.Environment);
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

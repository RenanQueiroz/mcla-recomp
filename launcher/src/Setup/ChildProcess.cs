// Running a program for the setup and reading what it prints line by line (progress lines that end with a
// carriage return, as git and curl write them, count as lines). No window, no prompts; killed with its
// children when the setup is cancelled.
using System.Diagnostics;
using System.Text;

namespace MclaLauncher.Setup;

/// The git settings of every git the launcher starts, and of the programs those start, given in the child's environment so that
/// nobody's git configuration is changed. The same list is in Set-GitEnvironment (scripts\mcla-lib.ps1), which the scripts use:
/// keep the two alike (scripts\ci\check-git-env.ps1 compares them).
///
/// Git reads no setting of the PC: not the system config (for MinGit, its own etc\gitconfig, which includes an installed Git
/// for Windows' etc\gitconfig) and not the user's ~\.gitconfig. A certificate file that http.sslcainfo named in one of them and
/// that was gone (a half-removed Git for Windows) stopped the setup at "Getting the source" with "error adding trust anchors
/// from file: C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt" (issues #32, #71). What is set instead:
///  - http.sslBackend=schannel: HTTPS is checked against Windows' own certificate store, which is what MinGit does when nothing
///    is set. Said out loud, git does not hand a certificate file (a GIT_SSL_CAINFO in the environment, an http.sslCAInfo in
///    the repository) to Windows' secure channel, which is what failed above.
///  - safe.directory=*: git refuses a repository whose folder it can't show to be the user's ("detected dubious ownership",
///    exit code 128): on a drive that keeps no owner (exFAT, FAT32), in a folder owned by another account or by
///    Administrators, or on a network path. After the setup's clone, every git command in the checkout is then refused (issue
///    #64). The checkouts git is run on here are the ones the launcher made itself, so they are trusted. git reads
///    safe.directory from the environment and from the command line, never from the repository.
///  - core.autocrlf=true, core.symlinks=false, core.fscache=true: MinGit's own defaults, which skipping the system config would
///    take away (its etc\gitconfig sets the first two, and the Git for Windows config it includes the third). Every SDK tree on a
///    player's PC was checked out with autocrlf=true, so its files have CRLF line endings, and the setup's LF patches are matched
///    against them by git apply only when git converts the file's line endings the same way ("patch does not apply" without it).
///    Fresh checkouts come out the same. No credential helper and no LFS filter: the setup reads public repositories over HTTPS.
static class GitEnvironment
{
    // NUL: git reads no file for that scope (the Windows name of /dev/null; GIT_CONFIG_GLOBAL needs git 2.32, MinGit is newer).
    const string NoFile = "NUL";

    static readonly (string Key, string Value)[] Settings =
    [
        ("http.sslBackend", "schannel"), ("safe.directory", "*"),
        ("core.autocrlf", "true"), ("core.symlinks", "false"), ("core.fscache", "true"),
    ];

    public static void Apply(IDictionary<string, string?> environment)
    {
        environment["GIT_CONFIG_NOSYSTEM"] = "1";
        environment["GIT_CONFIG_GLOBAL"] = NoFile;
        // Settings already in GIT_CONFIG_COUNT stay: these are added after them.
        var count = environment.TryGetValue("GIT_CONFIG_COUNT", out var text) && int.TryParse(text, out var n) && n >= 0 ? n : 0;
        foreach (var (key, value) in Settings)
        {
            environment[$"GIT_CONFIG_KEY_{count}"] = key;
            environment[$"GIT_CONFIG_VALUE_{count}"] = value;
            count++;
        }
        environment["GIT_CONFIG_COUNT"] = count.ToString();
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
        GitEnvironment.Apply(psi.Environment);
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

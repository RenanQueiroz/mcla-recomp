// Checks of the git settings of the launcher's own git calls (fork issues #32 and #71: the setup's MinGit still read the system config
// of an installed, half-removed Git for Windows, whose http.sslcainfo named a certificate file that was gone, and "Getting the source"
// failed with "error adding trust anchors from file"). GitEnvironment (Setup\ChildProcess.cs) gives every git the launcher starts the
// environment that makes it read none of the PC's git settings. scripts\ci\check-git-env.ps1 tries the same for the scripts, and
// compares the two lists. Nothing here writes outside <scratch>; the process's environment is changed for the checks and put back.
//   git-config <scratch> [--no-network]
//       a PC whose git settings are broken, in this process's environment: a system config that includes another one that names a missing
//       certificate file (as MinGit's etc\gitconfig includes Git for Windows' own), a user config, an XDG config and a GIT_SSL_CAINFO.
//       A plain git reads all of them; through Proc.RunAsync (status reads, update) and ChildProcess.RunAsync (the setup's clone) `git
//       config --show-scope --show-origin --list` has no system or global entry, MinGit's own defaults are given on the command line (core.autocrlf=true,
//       core.symlinks=false, core.fscache=true: an LF patch applies to a CRLF working file, as the SDK trees of the players have, and does not when only
//       the system config is skipped), and `git ls-remote --heads` of the fork works (the one read-only network call; --no-network leaves it out)
using System.Diagnostics;
using System.Text;
using MclaLauncher.Setup;

namespace MclaLauncher;

static partial class Program
{
    static async Task GitConfigChecks(string scratch, bool network)
    {
        scratch = Path.GetFullPath(scratch);
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(Path.Combine(scratch, "home"));
        Directory.CreateDirectory(Path.Combine(scratch, "xdg", "git"));
        Directory.CreateDirectory(Path.Combine(scratch, "work"));
        static string Forward(string path) => path.Replace('\\', '/');
        var gone = Forward(Path.Combine(scratch, "gone", "ca-bundle.crt"));
        File.WriteAllText(Path.Combine(scratch, "fake-gfw-gitconfig"), $"[http]\n\tsslCAInfo = {gone}\n[user]\n\tname = fake-system\n");
        File.WriteAllText(Path.Combine(scratch, "fake-system-gitconfig"), $"[include]\n\tpath = {Forward(Path.Combine(scratch, "fake-gfw-gitconfig"))}\n");
        File.WriteAllText(Path.Combine(scratch, "home", ".gitconfig"), $"[http]\n\tsslCAInfo = {gone}\n[user]\n\temail = fake-home\n");
        File.WriteAllText(Path.Combine(scratch, "xdg", "git", "config"), $"[http]\n\tsslCAInfo = {gone}\n[user]\n\tsigningkey = fake-xdg\n");

        string[] names = ["GIT_CONFIG_SYSTEM", "GIT_CONFIG_GLOBAL", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_COUNT", "GIT_SSL_CAINFO", "GIT_TERMINAL_PROMPT", "HOME", "XDG_CONFIG_HOME"];
        var saved = names.ToDictionary(n => n, Environment.GetEnvironmentVariable);
        var originalDirectory = Environment.CurrentDirectory;
        try
        {
            foreach (var name in new[] { "GIT_CONFIG_GLOBAL", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_COUNT" }) Environment.SetEnvironmentVariable(name, null);
            Environment.SetEnvironmentVariable("GIT_CONFIG_SYSTEM", Path.Combine(scratch, "fake-system-gitconfig"));
            Environment.SetEnvironmentVariable("HOME", Path.Combine(scratch, "home"));
            Environment.SetEnvironmentVariable("XDG_CONFIG_HOME", Path.Combine(scratch, "xdg"));
            Environment.SetEnvironmentVariable("GIT_SSL_CAINFO", gone);
            Environment.SetEnvironmentVariable("GIT_TERMINAL_PROMPT", "0");
            Environment.CurrentDirectory = Path.Combine(scratch, "work");

            var gitExe = Paths.GitExe;
            Console.WriteLine($"      git: {gitExe}");
            string[] listArgs = ["config", "--show-scope", "--show-origin", "--list"];

            // The broken PC: a plain git reads all of it.
            var (plainCode, plain) = RunPlainGit(gitExe, listArgs);
            Check("(control) a plain git reads the fake system config (through an include), the user config and the XDG config",
                  plainCode == 0 && plain.Contains("fake-system") && plain.Contains("fake-home") && plain.Contains("fake-xdg"), plain.Replace('\n', '|'));

            static bool Foreign(string line) => line.StartsWith("system\t") || line.StartsWith("global\t") || line.Contains("fake-system") || line.Contains("fake-home") || line.Contains("fake-xdg");

            // The launcher's status reads and update (Proc.RunAsync, which Paths.Prepare gives the settings).
            var viaProc = await Proc.RunAsync("git", listArgs, TimeSpan.FromSeconds(30));
            var procLines = viaProc.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
            Check("Proc.RunAsync: git reads no system or global setting, and none of the fake ones", viaProc.ExitCode == 0 && !procLines.Any(Foreign), string.Join("|", procLines.Where(Foreign)));
            Check("Proc.RunAsync: the command line's settings are there (HTTPS by Windows, the checkouts trusted)",
                  procLines.Any(l => l.StartsWith("command\t") && l.EndsWith("http.sslbackend=schannel")) && procLines.Any(l => l.StartsWith("command\t") && l.EndsWith("safe.directory=*")),
                  string.Join("|", procLines.Where(l => l.StartsWith("command\t"))));
            // MinGit's own defaults, which skipping its system config would take away: CRLF in the working files (every SDK tree on a player's PC was
            // checked out so, and the setup's LF patches only match them that way) and symlinks as small files.
            Check("Proc.RunAsync: MinGit's own defaults are given (core.autocrlf=true, core.symlinks=false, core.fscache=true)",
                  procLines.Any(l => l.StartsWith("command\t") && l.EndsWith("core.autocrlf=true")) && procLines.Any(l => l.StartsWith("command\t") && l.EndsWith("core.symlinks=false"))
                  && procLines.Any(l => l.StartsWith("command\t") && l.EndsWith("core.fscache=true")), string.Join("|", procLines.Where(l => l.StartsWith("command\t"))));

            // The setup (the clone, the checks of the checkout: ChildProcess.RunAsync).
            var viaChild = new List<string>();
            var childCode = await ChildProcess.RunAsync(gitExe, listArgs, Path.Combine(scratch, "work"), null, line => { lock (viaChild) viaChild.Add(line.Trim()); }, CancellationToken.None);
            Check("ChildProcess.RunAsync: git reads no system or global setting, and none of the fake ones", childCode == 0 && !viaChild.Any(Foreign), string.Join("|", viaChild.Where(Foreign)));
            Check("ChildProcess.RunAsync: the command line's settings are there",
                  viaChild.Any(l => l.StartsWith("command\t") && l.EndsWith("http.sslbackend=schannel")) && viaChild.Any(l => l.StartsWith("command\t") && l.EndsWith("safe.directory=*"))
                  && viaChild.Any(l => l.StartsWith("command\t") && l.EndsWith("core.autocrlf=true")) && viaChild.Any(l => l.StartsWith("command\t") && l.EndsWith("core.symlinks=false")),
                  string.Join("|", viaChild.Where(l => l.StartsWith("command\t"))));

            // The regression those defaults are for: a working file with CRLF line endings (what an autocrlf=true checkout makes) and an LF patch, as the
            // SDK's patches are. git apply matches them only when it converts the file the way that checkout did. With the system config skipped and
            // nothing else (what 625256b did) it does not; through ChildProcess.RunAsync it does.
            var crlfRepo = Path.Combine(scratch, "crlf-repo");
            Directory.CreateDirectory(crlfRepo);
            var patchFile = Path.Combine(scratch, "lf.patch");
            File.WriteAllBytes(Path.Combine(crlfRepo, "config.h"), Encoding.ASCII.GetBytes("line one\r\nline two\r\nline three\r\n"));
            File.WriteAllBytes(patchFile, Encoding.ASCII.GetBytes("--- a/config.h\n+++ b/config.h\n@@ -1,3 +1,3 @@\n line one\n-line two\n+line 2\n line three\n"));
            var skipOnly = new Dictionary<string, string> { ["GIT_CONFIG_NOSYSTEM"] = "1", ["GIT_CONFIG_GLOBAL"] = "NUL" };
            RunPlainGit(gitExe, ["-C", crlfRepo, "init", "-q"], skipOnly);
            string[] applyArgs = ["-C", crlfRepo, "apply", "--check", patchFile];
            var (withoutCode, withoutText) = RunPlainGit(gitExe, applyArgs, skipOnly);
            var applyOutput = new List<string>();
            var withCode = await ChildProcess.RunAsync(gitExe, applyArgs, null, null, line => { lock (applyOutput) applyOutput.Add(line.Trim()); }, CancellationToken.None);
            Check("an LF patch fails on a CRLF working file when only the system config is skipped (the control)", withoutCode != 0, $"exit {withoutCode}: {withoutText.Trim().Replace('\n', '|')}");
            Check("an LF patch applies to a CRLF working file through ChildProcess.RunAsync (core.autocrlf=true)", withCode == 0, $"exit {withCode}: {string.Join("|", applyOutput)}");

            // The settings the environment already carries stay, with these added after them.
            var env = new Dictionary<string, string?> { ["GIT_CONFIG_COUNT"] = "1", ["GIT_CONFIG_KEY_0"] = "core.longpaths", ["GIT_CONFIG_VALUE_0"] = "true" };
            GitEnvironment.Apply(env);
            Check("GitEnvironment.Apply keeps the entries already in GIT_CONFIG_COUNT and adds its own after them",
                  env["GIT_CONFIG_COUNT"] == "6" && env["GIT_CONFIG_KEY_0"] == "core.longpaths" && env["GIT_CONFIG_KEY_1"] == "http.sslBackend" && env["GIT_CONFIG_VALUE_1"] == "schannel"
                  && env["GIT_CONFIG_KEY_2"] == "safe.directory" && env["GIT_CONFIG_VALUE_2"] == "*" && env["GIT_CONFIG_NOSYSTEM"] == "1" && env["GIT_CONFIG_GLOBAL"] == "NUL"
                  && env["GIT_CONFIG_KEY_3"] == "core.autocrlf" && env["GIT_CONFIG_VALUE_3"] == "true" && env["GIT_CONFIG_KEY_4"] == "core.symlinks" && env["GIT_CONFIG_VALUE_4"] == "false"
                  && env["GIT_CONFIG_KEY_5"] == "core.fscache" && env["GIT_CONFIG_VALUE_5"] == "true",
                  string.Join(", ", env.Select(p => $"{p.Key}={p.Value}")));

            if (!network) { Console.WriteLine("      (the network call is left out: --no-network)"); return; }

            // The one read-only network call: the fork's heads. HTTPS is still checked, by Windows.
            var url = $"https://github.com/{ReleaseSource.Repository}.git";
            var (controlCode, control) = RunPlainGit(gitExe, ["ls-remote", "--heads", url, "main"]);
            if (controlCode != 0) Console.WriteLine($"      (control) a plain git fails on this PC, as it did for the players: {control.Trim().Split('\n').Last()}");
            else Console.WriteLine("      NOTE a plain git worked on the broken PC too (this git ignores the missing certificate file): the call below proves less");
            var remote = await Proc.RunAsync("git", ["ls-remote", "--heads", url, "main"], TimeSpan.FromSeconds(60));
            Check("Proc.RunAsync: git ls-remote --heads of the fork works on the broken PC", remote.ExitCode == 0 && remote.Output.Contains("refs/heads/main"), remote.ExitCode == 0 ? remote.Output.Trim() : remote.Error.Trim());
        }
        finally
        {
            Environment.CurrentDirectory = originalDirectory;
            foreach (var name in names) Environment.SetEnvironmentVariable(name, saved[name]);
        }
    }

    /// git with this process's environment as it is (plus `extra`), the way a start without the launcher's settings would run it.
    static (int Code, string Output) RunPlainGit(string gitExe, string[] args, IReadOnlyDictionary<string, string>? extra = null)
    {
        var psi = new ProcessStartInfo(gitExe)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var arg in args) psi.ArgumentList.Add(arg);
        if (extra != null) foreach (var pair in extra) psi.Environment[pair.Key] = pair.Value;
        using var process = Process.Start(psi)!;
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(60_000)) { try { process.Kill(true); } catch (InvalidOperationException) { } }
        return (process.ExitCode, output.GetAwaiter().GetResult() + error.GetAwaiter().GetResult());
    }
}

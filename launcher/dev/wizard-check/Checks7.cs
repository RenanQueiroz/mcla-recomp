// Checks of how the launcher starts Windows PowerShell (fork issue 83): the bare name "powershell.exe" is found only through a PATH
// entry (C:\Windows\System32\WindowsPowerShell\v1.0), and some Windows tweak tools remove that entry, so the setup stopped with
// "An error occurred trying to start process 'powershell.exe' ... The system cannot find the file specified". The launcher now
// starts it by its full path (WindowsPowerShell in Backend.cs) and falls back to the bare name, saying so, only when the file is
// not at its usual place. Nothing here writes outside <scratch>; the process's own PATH is changed for the start checks and put back.
//   powershell <scratch>    the resolved path on this PC (absolute, exists); the fallback with an injected system folder that
//                           has no PowerShell; a start with the PATH lacking PowerShell's folder, the old way and the new way,
//                           and a real BuildRun of a small build-game.ps1 in that state; no source file starting it by its bare name
using System.ComponentModel;
using System.Diagnostics;
using System.Text.RegularExpressions;
using MclaLauncher.Setup;

namespace MclaLauncher;

static partial class Program
{
    static void PowerShellChecks(string scratch)
    {
        scratch = Path.GetFullPath(scratch);
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);

        // This PC: the real system folder.
        var usualHere = Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe");
        var exe = WindowsPowerShell.Locate(out var note);
        Console.WriteLine($"      Windows PowerShell: {exe}");
        Check("the file to start is an absolute path", Path.IsPathFullyQualified(exe), exe);
        Check("it is powershell.exe under the system folder's WindowsPowerShell\\v1.0", string.Equals(exe, usualHere, StringComparison.OrdinalIgnoreCase), exe);
        Check("the file exists on this PC", File.Exists(exe));
        Check("nothing is said when it is where it should be", note == null, note ?? "");

        // An injected system folder: with the file, without the folder, with the folder but not the file, with a folder where the
        // file should be, and none given.
        var withFile = Path.Combine(scratch, "system-with");
        Directory.CreateDirectory(Path.Combine(withFile, @"WindowsPowerShell\v1.0"));
        File.WriteAllText(Path.Combine(withFile, @"WindowsPowerShell\v1.0\powershell.exe"), "");
        var found = WindowsPowerShell.Resolve(withFile, out var foundNote);
        Check("an injected system folder that has the file: its full path, nothing said",
              string.Equals(found, Path.Combine(withFile, @"WindowsPowerShell\v1.0\powershell.exe"), StringComparison.OrdinalIgnoreCase) && foundNote == null, found);

        var missing = Path.Combine(scratch, "system-without");
        var fallback = WindowsPowerShell.Resolve(missing, out var fallbackNote);
        Console.WriteLine($"      {fallbackNote}");
        Check("a system folder without PowerShell: the bare name", fallback == "powershell.exe", fallback);
        Check("and it says so, naming the place it looked and the PATH",
              fallbackNote != null && fallbackNote.Contains("Windows PowerShell was not found at its usual place")
              && fallbackNote.Contains(Path.Combine(missing, @"WindowsPowerShell\v1.0\powershell.exe")) && fallbackNote.Contains("PATH"), fallbackNote ?? "(nothing)");
        Check("the note is one plain line", fallbackNote != null && !fallbackNote.Contains('\n') && !fallbackNote.Contains("Exception"));

        var folderOnly = Path.Combine(scratch, "system-folder-only");
        Directory.CreateDirectory(Path.Combine(folderOnly, @"WindowsPowerShell\v1.0"));
        Check("the folder without the file: the bare name, with a note",
              WindowsPowerShell.Resolve(folderOnly, out var n3) == "powershell.exe" && n3 != null);

        var folderAsFile = Path.Combine(scratch, "system-folder-as-file");
        Directory.CreateDirectory(Path.Combine(folderAsFile, @"WindowsPowerShell\v1.0\powershell.exe"));
        Check("a folder named powershell.exe is no PowerShell: the bare name, with a note",
              WindowsPowerShell.Resolve(folderAsFile, out var n4) == "powershell.exe" && n4 != null);

        foreach (var none in new string?[] { null, "", " " })
        {
            var r = WindowsPowerShell.Resolve(none, out var n5);
            Check($"no system folder ({(none == null ? "null" : $"\"{none}\"")}): the bare name, with a note, no exception", r == "powershell.exe" && n5 != null && n5.Contains("usual place"));
        }

        // A start with the PATH lacking PowerShell's folder: the state of the PC in the issue. CreateProcess finds a bare name in the
        // exe's folder, the current folder, System32, the Windows folder and then the PATH of this process, so this process's PATH is
        // changed (and put back) rather than the child's.
        var originalPath = Environment.GetEnvironmentVariable("PATH") ?? "";
        static bool HoldsPowerShell(string entry)
        {
            try { return File.Exists(Path.Combine(Environment.ExpandEnvironmentVariables(entry.Trim().Trim('"')), "powershell.exe")); }
            catch (ArgumentException) { return false; }
        }
        var entries = originalPath.Split(';', StringSplitOptions.RemoveEmptyEntries);
        var onPathHere = entries.Any(HoldsPowerShell);
        try
        {
            Environment.SetEnvironmentVariable("PATH", string.Join(';', entries.Where(e => !HoldsPowerShell(e))));
            Check("(set-up) no folder on the PATH holds powershell.exe now", !(Environment.GetEnvironmentVariable("PATH") ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries).Any(HoldsPowerShell));

            var oldWay = "";
            try
            {
                var psi = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
                foreach (var a in new[] { "-NoProfile", "-NonInteractive", "-Command", "exit 0" }) psi.ArgumentList.Add(a);
                using var p = Process.Start(psi)!;
                p.WaitForExit(30_000);
                oldWay = "it started";
            }
            catch (Win32Exception ex) { oldWay = ex.NativeErrorCode == 2 ? "file not found (2)" : $"Win32 error {ex.NativeErrorCode}"; }
            Check("the bare name does not start without PowerShell's folder on the PATH (the issue)", oldWay == "file not found (2)", oldWay);

            var path = WindowsPowerShell.Locate(out var noteNow);
            var (code, output) = RunPowerShell(path, "Write-Output ('PS' + $PSVersionTable.PSVersion.Major); exit 7");
            Check("the full path starts it all the same, and its exit code comes back", code == 7 && output.Trim() == "PS5", $"exit {code}, {output.Trim()}");
            Check("with nothing said", noteNow == null);

            // The fallback, in the state of the issue with no usable file: the error is the one the player saw; the note is what explains it.
            var gone = WindowsPowerShell.Resolve(missing, out _);
            string failure;
            try { RunPowerShell(gone, "exit 0"); failure = "it started"; }
            catch (Win32Exception ex) { failure = ex.NativeErrorCode == 2 ? "file not found (2)" : $"Win32 error {ex.NativeErrorCode}"; }
            Check("with PowerShell neither at its usual place nor on the PATH the start fails as it did, with the note to log", failure == "file not found (2)" && fallbackNote != null, failure);

            // The launcher's own start: a real BuildRun of a small build-game.ps1, in that state.
            var launcherDir = Path.Combine(scratch, "launcher");
            Directory.CreateDirectory(launcherDir);
            File.WriteAllText(Path.Combine(launcherDir, "build-game.ps1"), "Write-Output ('build-game.ps1 ran in Windows PowerShell ' + $PSVersionTable.PSVersion.Major)\r\nexit 0\r\n");
            var lines = new List<string>();
            var run = new BuildRun(BuildKind.Build, line => { lock (lines) lines.Add(line); }, new BuildRunOptions(launcherDir, Path.Combine(launcherDir, "logs"), []));
            var finished = run.Completion.Wait(TimeSpan.FromSeconds(60));
            if (!finished) run.Stop();
            string[] seen;
            lock (lines) seen = lines.ToArray();
            Console.WriteLine($"      BuildRun output: {string.Join(" | ", seen)}");
            Check("BuildRun starts it with the PATH lacking PowerShell's folder: the script runs and ends 0", finished && run.Completion.Result == 0
                  && seen.Any(l => l.Contains("build-game.ps1 ran in Windows PowerShell 5")), finished ? $"exit {run.Completion.Result}" : "timed out");
            Check("and its log says nothing about a missing PowerShell", !seen.Any(l => l.Contains("not found at its usual place")));
            Check("the run's log file has the line", File.Exists(run.LogFile) && File.ReadAllText(run.LogFile).Contains("ran in Windows PowerShell"));
        }
        finally
        {
            Environment.SetEnvironmentVariable("PATH", originalPath);
        }

        // With the PATH as it was, the fallback's bare name still starts PowerShell where the PATH has it (a PC whose System32 is
        // somewhere unusual); skipped where the PATH has none.
        if (onPathHere)
        {
            var bare = WindowsPowerShell.Resolve(missing, out _);
            var (code, _) = RunPowerShell(bare, "exit 3");
            Check("the fallback's bare name starts PowerShell through the PATH where the PATH has it", code == 3, $"exit {code}");
        }
        else Console.WriteLine("      (the PATH here has no PowerShell folder: the bare-name start through the PATH was not tried)");

        // No source file of the launcher starts it by its bare name any more (only the fallback constant in Backend.cs may name it).
        var repo = FindRepoFolder();
        if (repo == null) { Console.WriteLine("      (the source folders were not found from here: the scan for a bare \"powershell.exe\" was skipped)"); return; }
        var bareInCode = new List<string>();
        foreach (var file in Directory.EnumerateFiles(Path.Combine(repo, "launcher", "src"), "*.cs", SearchOption.AllDirectories)
                     .Where(f => !f.Contains(@"\obj\") && !f.Contains(@"\bin\")))
        {
            var text = File.ReadAllLines(file);
            for (var i = 0; i < text.Length; i++)
                if (!text[i].TrimStart().StartsWith("//") && Regex.IsMatch(text[i], "\"powershell(\\.exe)?\"", RegexOptions.IgnoreCase) && !text[i].Contains("const string BareName"))
                    bareInCode.Add($"{Path.GetRelativePath(repo, file)}:{i + 1}");
        }
        Check("no launcher source starts \"powershell.exe\" by its bare name (only WindowsPowerShell.BareName names it)", bareInCode.Count == 0, string.Join(", ", bareInCode));
        var bareInScripts = new List<string>();
        foreach (var dir in new[] { Path.Combine(repo, "launcher"), Path.Combine(repo, "scripts") })
            foreach (var file in Directory.EnumerateFiles(dir, "*.ps1", SearchOption.TopDirectoryOnly))
            {
                var text = File.ReadAllLines(file);
                for (var i = 0; i < text.Length; i++)
                    if (!text[i].TrimStart().StartsWith('#') && Regex.IsMatch(text[i], @"(?<![\\\w.-])powershell(\.exe)?[""']?\s+-\w", RegexOptions.IgnoreCase))
                        bareInScripts.Add($"{Path.GetRelativePath(repo, file)}:{i + 1}");
            }
        Check("no script of the launcher or scripts\\ runs powershell by its bare name", bareInScripts.Count == 0, string.Join(", ", bareInScripts));
    }

    /// Runs PowerShell (the file given) with a command; its output and exit code.
    static (int Code, string Output) RunPowerShell(string exe, string command)
    {
        var psi = new ProcessStartInfo(exe) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
        foreach (var a in new[] { "-NoProfile", "-NonInteractive", "-Command", command }) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        var output = p.StandardOutput.ReadToEndAsync();
        if (!p.WaitForExit(30_000)) { try { p.Kill(true); } catch (InvalidOperationException) { } }
        return (p.ExitCode, output.GetAwaiter().GetResult());
    }

    /// The repository's folder, found upward from this program (launcher\dev\wizard-check\bin\...), or null.
    static string? FindRepoFolder()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
            if (File.Exists(Path.Combine(dir.FullName, "launcher", "src", "Backend.cs"))) return dir.FullName;
        return null;
    }
}

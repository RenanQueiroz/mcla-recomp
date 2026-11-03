// Checks of what the launcher and the setup say when Windows refuses to start the game (fork issue 55): Smart App Control, an App
// Control for Business policy or a group policy blocks the unsigned mcla.exe that was built on the PC (Win32 error 4551 or 1260).
// The real block can't be made without turning Smart App Control on, so the codes are made up (new Win32Exception(4551)); what
// is real here is the exception of a start that fails for another reason, which must not be taken for a block.
//   app-control <scratch>    AppControl.IsBlock / Message, and a failed start of ShaderPrepRun (its log keeps the raw text)
using System.ComponentModel;
using System.Diagnostics;
using MclaLauncher.Setup;

namespace MclaLauncher;

static partial class Program
{
    static void AppControlChecks(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);

        // Which errors count as a policy block.
        Check("4551 (ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION) is a block", AppControl.IsBlock(new Win32Exception(4551)));
        Check("1260 (ERROR_ACCESS_DISABLED_BY_POLICY) is a block", AppControl.IsBlock(new Win32Exception(1260)));
        Check("one inside a SetupException is found", AppControl.IsBlock(new SetupException("x", null, new Win32Exception(4551))));
        Check("one inside an InvalidOperationException is found", AppControl.IsBlock(new InvalidOperationException("x", new Win32Exception(1260))));
        Check("2 (file not found) is not a block", !AppControl.IsBlock(new Win32Exception(2)));
        Check("5 (access denied) is not a block", !AppControl.IsBlock(new Win32Exception(5)));
        Check("193 (not a valid Win32 program) is not a block", !AppControl.IsBlock(new Win32Exception(193)));
        Check("225 (a virus scanner's refusal) is not taken for a block", !AppControl.IsBlock(new Win32Exception(225)));
        Check("another exception type with the same number is not a block", !AppControl.IsBlock(new IOException("4551")));
        Check("null is not a block", !AppControl.IsBlock(null));

        // The words.
        var setup = AppControl.Message("Try again");
        var play = AppControl.Message("Play");
        Console.WriteLine($"      ({setup.Length} characters) {setup}");
        Check("the message names Smart App Control and the administrator policy", setup.Contains("Smart App Control") && setup.Contains("administrator"));
        Check("it says where to turn it off", setup.Contains("Windows Security > App & browser control > Smart App Control settings > Off"));
        Check("it says what to press after", setup.Contains("Off, then Try again.") && play.Contains("Off, then Play."));
        Check("it warns that it may not turn back on", setup.Contains("can't be turned back on later without reinstalling Windows"));
        Check("it is plain: one line, no exception text, no error number", !setup.Contains('\n') && !setup.Contains("Exception") && !setup.Contains("4551") && !setup.Contains("Win32"));

        // A start that fails for another reason: the exception stays what it is.
        Win32Exception? missing = null;
        try { using var p = Process.Start(new ProcessStartInfo(Path.Combine(scratch, "no-such-program.exe")) { UseShellExecute = false }); }
        catch (Win32Exception ex) { missing = ex; }
        Check("starting a missing program threw a Win32Exception", missing != null);
        Check("and it is not taken for a block", missing != null && !AppControl.IsBlock(missing), $"error {missing?.NativeErrorCode}");

        // ShaderPrepRun with a start that fails (no such folder for the exe): the exception goes to the caller as it is, the run's
        // log keeps the raw text, and the log isn't left open.
        var logs = Path.Combine(scratch, "logs");
        var exe = Path.Combine(scratch, "no-game-folder", "mcla.exe");
        Exception? thrown = null;
        try { _ = new ShaderPrepRun(exe, "", _ => { }, logs, reason: null); }
        catch (Exception ex) { thrown = ex; }
        Check("ShaderPrepRun passes the failed start on as a Win32Exception", thrown is Win32Exception, thrown == null ? "no exception" : thrown.GetType().Name);
        var logFile = Directory.Exists(logs) ? Directory.GetFiles(logs, "prepare-shaders-2*.log").FirstOrDefault() : null;
        Check("its log was written", logFile != null);
        if (logFile != null)
        {
            // Not held open: a second writer can take the file.
            try { using var again = new FileStream(logFile, FileMode.Open, FileAccess.ReadWrite, FileShare.None); Check("the log is closed", true); }
            catch (IOException ex) { Check("the log is closed", false, ex.Message); }
            var text = File.ReadAllText(logFile);
            Check("the log has the command line and Windows' own text", text.Contains("mcla.exe ") && text.Contains("could not start mcla.exe:") && text.Contains("Win32Exception"));
        }
        SafeFolder.Delete(scratch);
    }
}

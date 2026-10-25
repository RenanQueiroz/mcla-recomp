// Replacing an installed folder with a new one, with a way back.
//
// This file stands alone (nothing from the launcher, nothing but the BCL and a few Win32 calls), because
// three programs compile it: MCLA-Setup.exe (the installer and updater, NativeAOT), the launcher (to clear
// what an update leaves behind) and the harness launcher\dev\update-check.
//
// Why a folder and not the exe: the launcher is a folder (an exe, its DLLs, the Windows App SDK), and a
// running exe cannot overwrite itself. The update is made by another program, MCLA-Setup.exe, which waits for
// the launcher to close and then swaps folders with two renames:
//
//     <dir>                 the installed launcher            -> <dir>.old-<stamp>
//     <dir>.new-<stamp>     the new one, staged beside it     -> <dir>
//
// A rename is atomic and costs nothing however big the folder is, the new content is complete before the
// first rename, and until the second rename is done the old one is still there. If the new launcher does
// not start, the same two renames in the other direction put the old one back (RollBack). What is left
// (<dir>.old-*, .new-*, .failed-*) is removed by CleanLeftovers, at the end of the swap or on a later start.
// If the program is killed between the two renames, Recover puts the old folder back.
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace Mcla.Updating;

/// What a swap did, kept to commit it or to undo it.
public sealed class SwapRecord
{
    /// The folder that was replaced.
    public required string Target { get; init; }
    /// Where the previous content is now; null on a first install (nothing was there).
    public string? Previous { get; init; }
}

public static class FolderSwap
{
    public const string OldTag = ".old-";
    public const string NewTag = ".new-";
    public const string FailedTag = ".failed-";

    static string Trim(string folder) => Path.GetFullPath(folder).TrimEnd('\\', '/');

    static string Stamp() => DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Environment.ProcessId;

    /// A name for the staging folder beside `target` (same volume, so the swap is a rename).
    public static string NewStagingPath(string target) => Trim(target) + NewTag + Stamp();

    /// The leftovers of earlier swaps beside `target`.
    public static IEnumerable<string> Leftovers(string target)
    {
        target = Trim(target);
        var parent = Path.GetDirectoryName(target);
        if (parent == null || !Directory.Exists(parent)) yield break;
        var name = Path.GetFileName(target);
        foreach (var dir in Directory.EnumerateDirectories(parent, name + ".*"))
        {
            var tail = Path.GetFileName(dir)[name.Length..];
            if (tail.StartsWith(OldTag, StringComparison.Ordinal) || tail.StartsWith(NewTag, StringComparison.Ordinal)
                || tail.StartsWith(FailedTag, StringComparison.Ordinal))
                yield return dir;
        }
    }

    /// A swap that was killed between its two renames leaves no `target` and the previous folder as `.old-*`:
    /// the newest of those is put back. Returns true when it did.
    public static bool Recover(string target)
    {
        target = Trim(target);
        if (Directory.Exists(target)) return false;
        var old = Leftovers(target)
            .Where(d => Path.GetFileName(d)[Path.GetFileName(target).Length..].StartsWith(OldTag, StringComparison.Ordinal))
            .OrderByDescending(d => Directory.GetCreationTimeUtc(d)).FirstOrDefault();
        if (old == null) return false;
        RenameWithRetry(old, target, TimeSpan.FromSeconds(5));
        return true;
    }

    /// Removes the leftovers beside `target`; what is still in use stays for the next time. Returns how many
    /// folders went.
    public static int CleanLeftovers(string target, TimeSpan? patience = null)
    {
        var removed = 0;
        foreach (var dir in Leftovers(target).ToList())
            if (DeleteFolder(dir, patience ?? TimeSpan.Zero)) removed++;
        return removed;
    }

    /// Copies the named files (relative to the folders) from `from` to `to`, replacing; names that are not
    /// there are skipped. This is for files the user's setup wrote into the installed folder (the pointer to
    /// the install), which a new copy of the launcher does not have.
    public static void CopyKept(string from, string to, IEnumerable<string> names)
    {
        foreach (var name in names)
        {
            if (Path.IsPathRooted(name) || name.Contains("..")) continue;
            var source = Path.Combine(from, name);
            if (!File.Exists(source)) continue;
            var destination = Path.Combine(to, name);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(source, destination, overwrite: true);
        }
    }

    /// Puts `staged` in place of `target`. A first install is one rename; a replacement keeps the previous
    /// folder beside it as `.old-*`. If `target` cannot be renamed within `timeout` (a program has a file in it
    /// open), nothing has changed and an IOException says so.
    public static SwapRecord Apply(string target, string staged, TimeSpan timeout)
    {
        target = Trim(target);
        staged = Trim(staged);
        if (!Directory.Exists(staged)) throw new DirectoryNotFoundException($"the new folder is not there: {staged}");
        string? previous = null;
        if (Directory.Exists(target))
        {
            previous = target + OldTag + Stamp();
            RenameWithRetry(target, previous, timeout);
        }
        try
        {
            RenameWithRetry(staged, target, TimeSpan.FromSeconds(10));
        }
        catch
        {
            // The first rename was done and the second could not be: the old one goes back.
            if (previous != null) { try { Directory.Move(previous, target); } catch { /* Recover tries again later */ } }
            throw;
        }
        return new SwapRecord { Target = target, Previous = previous };
    }

    /// The new folder is good: the previous one is deleted (best effort; CleanLeftovers finishes it later).
    public static void Commit(SwapRecord swap, TimeSpan? patience = null)
    {
        if (swap.Previous != null) DeleteFolder(swap.Previous, patience ?? TimeSpan.FromSeconds(5));
    }

    /// The new folder is bad: it is moved aside as `.failed-*` (and deleted if it can be), and the previous
    /// one is put back. Returns false when there was nothing to put back (a first install).
    public static bool RollBack(SwapRecord swap)
    {
        var failed = swap.Target + FailedTag + Stamp();
        RenameWithRetry(swap.Target, failed, TimeSpan.FromSeconds(15));
        var restored = false;
        if (swap.Previous != null)
        {
            RenameWithRetry(swap.Previous, swap.Target, TimeSpan.FromSeconds(10));
            restored = true;
        }
        DeleteFolder(failed, TimeSpan.FromSeconds(3));
        return restored;
    }

    /// Directory.Move that waits while something has a file open (an IOException or UnauthorizedAccessException),
    /// as a scanner or a closing program does.
    public static void RenameWithRetry(string from, string to, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        var delay = 50;
        while (true)
        {
            try { Directory.Move(from, to); return; }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                if (DateTime.UtcNow >= deadline) throw new IOException($"could not rename {from} to {to}: {ex.Message}", ex);
                Thread.Sleep(delay);
                delay = Math.Min(delay * 2, 500);
            }
        }
    }

    /// Deletes a folder tree, retrying while something holds a file; false if it is still there.
    public static bool DeleteFolder(string folder, TimeSpan patience)
    {
        var deadline = DateTime.UtcNow + patience;
        while (true)
        {
            try
            {
                if (!Directory.Exists(folder)) return true;
                foreach (var file in Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories))
                {
                    var attributes = File.GetAttributes(file);
                    if ((attributes & FileAttributes.ReadOnly) != 0) File.SetAttributes(file, attributes & ~FileAttributes.ReadOnly);
                }
                Directory.Delete(folder, recursive: true);
                return true;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                if (DateTime.UtcNow >= deadline) return !Directory.Exists(folder);
                Thread.Sleep(200);
            }
        }
    }

    // ---- processes ------------------------------------------------------------------------------------------

    /// Waits for a process to end; true when it has (or was never there).
    public static bool WaitForProcess(int pid, TimeSpan timeout)
    {
        try
        {
            using var process = Process.GetProcessById(pid);
            return process.WaitForExit(timeout);
        }
        catch (ArgumentException) { return true; }
        catch (InvalidOperationException) { return true; }
    }

    /// The processes whose program file is below `folder` (the launcher, or anything run from there).
    public static List<Process> ProcessesUnder(string folder)
    {
        var prefix = Trim(folder) + "\\";
        var found = new List<Process>();
        foreach (var process in Process.GetProcesses())
        {
            var path = ImagePath(process.Id);
            if (path != null && path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) found.Add(process);
            else process.Dispose();
        }
        return found;
    }

    /// Waits until nothing runs from `folder`; false if something still does after `timeout`.
    public static bool WaitUntilFree(string folder, TimeSpan timeout, Func<bool>? cancelled = null)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (true)
        {
            var running = ProcessesUnder(folder);
            var any = running.Count > 0;
            foreach (var p in running) p.Dispose();
            if (!any) return true;
            if (DateTime.UtcNow >= deadline || cancelled?.Invoke() == true) return false;
            Thread.Sleep(250);
        }
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern nint OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(nint handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool QueryFullProcessImageNameW(nint process, uint flags, StringBuilder name, ref int size);

    /// The full path of a process's program, or null when it can't be read (another user's, protected).
    public static string? ImagePath(int pid)
    {
        var handle = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, false, pid);
        if (handle == 0) return null;
        try
        {
            var name = new StringBuilder(1024);
            var size = name.Capacity;
            return QueryFullProcessImageNameW(handle, 0, name, ref size) ? name.ToString(0, size) : null;
        }
        finally { CloseHandle(handle); }
    }
}

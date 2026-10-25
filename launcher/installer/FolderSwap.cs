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
//
// What this code may delete, and what it must never:
//  * A folder is the launcher's only when Setup made it: it holds installed.txt (OwnerMarker). The installer refuses
//    any other folder, and a launcher folder that holds anything Setup did not unpack (ForeignEntries: the player's
//    install put inside it, notes, a disc image), or whose install.json names an install inside it (InstallRootInside).
//  * The leftovers are named with an exact stamp (<dir>.old-20261024-231500-1234) and carry Setup's marker inside
//    (installed.txt in .old/.failed, StagingMarker in .new); a folder that merely has a similar name is not touched.
//  * Removing the launcher takes only the files Setup unpacked (DeleteOwned).
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Mcla.Updating;

/// What a swap did, kept to commit it or to undo it.
public sealed class SwapRecord
{
    /// The folder that was replaced.
    public required string Target { get; init; }
    /// Where the previous content is now; null on a first install (nothing was there).
    public string? Previous { get; init; }
}

/// A swap or a way back that could not be made. `OldFolderIsBack`: the folder is as it was before the swap (or there was
/// nothing to restore); when false the old launcher is in `Previous` and a run of MCLA-Setup.exe (Recover) puts it back.
public sealed class SwapFailedException(string message, string? previous, bool oldFolderIsBack, Exception? inner = null) : IOException(message, inner)
{
    public string? Previous { get; } = previous;
    public bool OldFolderIsBack { get; } = oldFolderIsBack;
}

public static class FolderSwap
{
    public const string OldTag = ".old-";
    public const string NewTag = ".new-";
    public const string FailedTag = ".failed-";
    /// Setup's mark in a launcher folder it made.
    public const string OwnerMarker = "installed.txt";
    /// The list of the files Setup unpacked into a launcher folder (one relative path with / per line).
    public const string ManifestName = "installed-files.txt";
    /// Written first into a staging folder, so that a leftover one can be told from a folder somebody else made.
    public const string StagingMarker = ".mcla-staging";
    /// Written into the folder a downloaded installer is kept in.
    public const string DownloadMarker = ".mcla-download";
    /// Names a launcher folder may hold besides the payload: Setup's own two, and what the launcher and the wizard write.
    public static readonly string[] StandardNames = [OwnerMarker, ManifestName, "install.json", "settings.json"];

    static string Trim(string folder) => Path.GetFullPath(folder).TrimEnd('\\', '/');

    static string Stamp() => DateTime.Now.ToString("yyyyMMdd-HHmmss") + "-" + Environment.ProcessId;

    /// A name for the staging folder beside `target` (same volume, so the swap is a rename).
    public static string NewStagingPath(string target) => Trim(target) + NewTag + Stamp();

    /// The staging folder, made, with its marker.
    public static string CreateStaging(string target)
    {
        var staged = NewStagingPath(target);
        Directory.CreateDirectory(staged);
        File.WriteAllText(Path.Combine(staged, StagingMarker), "Setup is unpacking a launcher here\n");
        return staged;
    }

    /// `.old-yyyyMMdd-HHmmss-pid`, and nothing else.
    static bool IsStamp(string text, string tag)
    {
        if (!text.StartsWith(tag, StringComparison.Ordinal)) return false;
        var parts = text[tag.Length..].Split('-');
        return parts.Length == 3
            && parts[0].Length == 8 && parts[0].All(char.IsAsciiDigit)
            && parts[1].Length == 6 && parts[1].All(char.IsAsciiDigit)
            && parts[2].Length is > 0 and <= 10 && parts[2].All(char.IsAsciiDigit);
    }

    /// The leftovers of earlier swaps beside `target`: named with an exact stamp, and with Setup's marker inside.
    public static IEnumerable<string> Leftovers(string target, string? onlyTag = null)
    {
        target = Trim(target);
        var parent = Path.GetDirectoryName(target);
        if (parent == null || !Directory.Exists(parent)) yield break;
        var name = Path.GetFileName(target);
        foreach (var dir in Directory.EnumerateDirectories(parent, name + ".*").ToList())
        {
            var tail = Path.GetFileName(dir)[name.Length..];
            var tag = IsStamp(tail, OldTag) ? OldTag : IsStamp(tail, NewTag) ? NewTag : IsStamp(tail, FailedTag) ? FailedTag : null;
            if (tag == null || (onlyTag != null && tag != onlyTag)) continue;
            var marker = tag == NewTag ? StagingMarker : OwnerMarker;
            if (!File.Exists(Path.Combine(dir, marker))) continue; // not ours: somebody's folder with a similar name
            yield return dir;
        }
    }

    /// A swap that was killed between its two renames leaves no `target` and the previous folder as `.old-*`:
    /// the newest of those is put back. Returns true when it did.
    public static bool Recover(string target)
    {
        target = Trim(target);
        if (Directory.Exists(target)) return false;
        var old = Leftovers(target, OldTag).OrderByDescending(d => Directory.GetCreationTimeUtc(d)).FirstOrDefault();
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

    // ---- what a launcher folder holds --------------------------------------------------------------------------------------

    /// The files Setup unpacked into `folder` (installed-files.txt), or null when the folder has no list.
    public static List<string>? ReadManifest(string folder)
    {
        var file = Path.Combine(folder, ManifestName);
        if (!File.Exists(file)) return null;
        return File.ReadAllLines(file).Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith('#')).ToList();
    }

    /// The first part of each relative path, and the names a launcher folder may hold besides (StandardNames, `keep`).
    public static HashSet<string> TopLevelNames(IEnumerable<string> relativePaths, IEnumerable<string>? keep = null)
    {
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var path in relativePaths) names.Add(path.Split('/', '\\')[0]);
        foreach (var name in StandardNames) names.Add(name);
        foreach (var name in keep ?? []) names.Add(name.Split('/', '\\')[0]);
        return names;
    }

    /// The entries of `folder` (names, not paths) that are not in `owned`: what somebody else put there.
    public static List<string> ForeignEntries(string folder, ISet<string> owned) =>
        Directory.EnumerateFileSystemEntries(folder).Select(Path.GetFileName).Where(n => n != null && !owned.Contains(n!)).Select(n => n!).OrderBy(n => n).ToList();

    /// The install folder that `<folder>\install.json` points at, when it lies inside `folder` (or is it): moving or deleting
    /// the launcher's folder would take the player's install with it. Null when it does not, or there is no pointer.
    public static string? InstallRootInside(string folder)
    {
        try
        {
            var file = Path.Combine(folder, "install.json");
            if (!File.Exists(file)) return null;
            using var doc = JsonDocument.Parse(File.ReadAllBytes(file), new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
            if (doc.RootElement.ValueKind != JsonValueKind.Object) return null;
            foreach (var property in doc.RootElement.EnumerateObject())
            {
                if (!property.Name.Equals("installRoot", StringComparison.OrdinalIgnoreCase) || property.Value.ValueKind != JsonValueKind.String) continue;
                var root = property.Value.GetString();
                if (string.IsNullOrWhiteSpace(root)) return null;
                var full = Trim(root);
                var inside = Trim(folder);
                return full.Equals(inside, StringComparison.OrdinalIgnoreCase) || full.StartsWith(inside + "\\", StringComparison.OrdinalIgnoreCase) ? full : null;
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or ArgumentException or NotSupportedException) { }
        return null;
    }

    /// True for a junction or a symbolic link (what a rename or a delete would act on is the link, not what it points at, but
    /// nothing here should be pointed at a link on purpose).
    public static bool IsLink(string path)
    {
        try { return (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }

    /// Removes only the files Setup unpacked (`relativePaths`), the standard names, and the folders that are then empty; whatever else
    /// is in `folder` stays, the folder with it. Returns the names left in the folder (empty when the folder itself is gone).
    public static List<string> DeleteOwned(string folder, IEnumerable<string> relativePaths, TimeSpan patience)
    {
        folder = Trim(folder);
        var paths = relativePaths.Where(p => p.Length > 0 && !Path.IsPathRooted(p) && !p.Contains("..")).ToList();
        var directories = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var relative in paths.Concat(StandardNames))
        {
            var full = Path.Combine(folder, relative.Replace('/', '\\'));
            DeleteFile(full, patience);
            for (var dir = Path.GetDirectoryName(full); dir != null && dir.Length > folder.Length; dir = Path.GetDirectoryName(dir)) directories.Add(dir);
        }
        foreach (var dir in directories.OrderByDescending(d => d.Length))
        {
            try { if (Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }
        var left = Directory.Exists(folder) ? ForeignEntries(folder, new HashSet<string>()) : [];
        if (left.Count == 0 && Directory.Exists(folder))
        {
            try { Directory.Delete(folder); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }
        return left;
    }

    static void DeleteFile(string path, TimeSpan patience)
    {
        var deadline = DateTime.UtcNow + patience;
        while (true)
        {
            try
            {
                if (!File.Exists(path)) return;
                var attributes = File.GetAttributes(path);
                if ((attributes & FileAttributes.ReadOnly) != 0) File.SetAttributes(path, attributes & ~FileAttributes.ReadOnly);
                File.Delete(path);
                return;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                if (DateTime.UtcNow >= deadline) throw new IOException($"could not delete {path}: {ex.Message}", ex);
                Thread.Sleep(200);
            }
        }
    }

    // ---- the swap ---------------------------------------------------------------------------------------------------------

    /// Puts `staged` in place of `target`. A first install is one rename; a replacement keeps the previous
    /// folder beside it as `.old-*`. If `target` cannot be renamed within `timeout` (a program has a file in it
    /// open), nothing has changed and an IOException says so. If the staged folder cannot be put in place after
    /// the old one was moved aside, the old one is put back (tried for as long as it takes, up to half a minute); only if
    /// that fails too is the folder missing, which SwapFailedException says (OldFolderIsBack false) and a run of
    /// MCLA-Setup.exe repairs (Recover). The staged folder is never touched here when that happens.
    public static SwapRecord Apply(string target, string staged, TimeSpan timeout)
    {
        target = Trim(target);
        staged = Trim(staged);
        if (!Directory.Exists(staged)) throw new DirectoryNotFoundException($"the new folder is not there: {staged}");
        try { File.Delete(Path.Combine(staged, StagingMarker)); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        string? previous = null;
        if (Directory.Exists(target))
        {
            previous = target + OldTag + Stamp();
            RenameWithRetry(target, previous, timeout);
        }
        try
        {
            RenameWithRetry(staged, target, timeout);
        }
        catch (Exception ex)
        {
            // The staged folder keeps its marker, so that a later clean-up knows it.
            try { File.WriteAllText(Path.Combine(staged, StagingMarker), "Setup is unpacking a launcher here\n"); } catch (Exception inner) when (inner is IOException or UnauthorizedAccessException) { }
            if (previous == null) throw;
            try { RenameWithRetry(previous, target, TimeSpan.FromSeconds(30)); }
            catch (Exception back)
            {
                throw new SwapFailedException($"The launcher's folder could not be put back after the swap failed ({back.Message}); the previous launcher is in {previous}. Run MCLA-Setup.exe again: it puts it back.", previous, false, ex);
            }
            throw new SwapFailedException($"The new launcher could not be put in place ({ex.Message}); the previous one is back.", previous, true, ex);
        }
        return new SwapRecord { Target = target, Previous = previous };
    }

    /// The new folder is good: the previous one is deleted (best effort; CleanLeftovers finishes it later).
    public static void Commit(SwapRecord swap, TimeSpan? patience = null)
    {
        if (swap.Previous != null) DeleteFolder(swap.Previous, patience ?? TimeSpan.FromSeconds(5));
    }

    /// The new folder is bad: it is moved aside as `.failed-*` (and deleted if it can be), and the previous
    /// one is put back. Returns false when there was nothing to put back (a first install). It does nothing at all, and says so,
    /// when the previous folder is no longer there: the player is never left with neither folder.
    public static bool RollBack(SwapRecord swap)
    {
        if (swap.Previous != null && !Directory.Exists(swap.Previous))
            throw new SwapFailedException($"The previous launcher is gone ({swap.Previous}), so the new one was left in place.", swap.Previous, false);
        var failed = swap.Target + FailedTag + Stamp();
        RenameWithRetry(swap.Target, failed, TimeSpan.FromSeconds(15));
        var restored = false;
        if (swap.Previous != null)
        {
            try { RenameWithRetry(swap.Previous, swap.Target, TimeSpan.FromSeconds(30)); }
            catch (Exception ex)
            {
                // Put the new one back where it was: a folder is better than none.
                try { RenameWithRetry(failed, swap.Target, TimeSpan.FromSeconds(10)); } catch (Exception) { }
                throw new SwapFailedException($"The previous launcher could not be put back ({ex.Message}); it is in {swap.Previous}.", swap.Previous, false, ex);
            }
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

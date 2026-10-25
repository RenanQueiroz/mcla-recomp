// Removing an install: only what the setup created, in the install folder it recorded; never what it did
// not (the game's own folder when it was used where it is, the player's saves and shader cache, anything else
// in the install folder); never through a link; never while a setup or the game runs from it.
using System.Diagnostics;

namespace MclaLauncher.Setup;

sealed record CleanupResult(IReadOnlyList<string> Removed, IReadOnlyList<string> Kept, string Summary);

static class InstallCleanup
{
    /// Removes the install (and the pointer beside the exe, if given). It deletes only the names that install.json
    /// lists as made by the setup (and a save's leftover .tmp), and refuses: a folder with no install.json of the
    /// setup's own, a list with a name that is not a plain name in that folder, a folder that is a drive, a link, the
    /// user's profile or one of its known folders, Windows or Program Files or a folder holding any of them, a game
    /// kept inside what would be deleted, a setup running in it, or the game running from it.
    public static CleanupResult Uninstall(string root, string? exeDirectory)
    {
        var layout = new InstallLayout(Path.GetFullPath(root));
        var record = InstallRecord.Load(layout.RecordFile);
        if (record == null || !string.Equals(Path.GetFullPath(record.InstallRoot).TrimEnd('\\'), layout.Root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            throw new SetupException($"{layout.Root} is not an install made by the setup (it has no install.json of its own), so nothing was removed.");
        if (Refusal(layout.Root) is { } refusal) throw new SetupException($"{refusal} Nothing was removed.");
        if (InstallLock.IsHeld(layout.Root))
            throw new SetupException("A setup is running in that folder. Nothing was removed.");
        if (GameRunningFrom(layout.Root))
            throw new SetupException("The game is running from that folder; close it first. Nothing was removed.");

        var owned = record.Owned;
        foreach (var name in owned)
            if (string.IsNullOrEmpty(name) || name != Path.GetFileName(name) || name is "." or ".." || name.Contains(':'))
                throw new SetupException($"install.json lists a name that is not a plain name in the install folder ({name}). Nothing was removed.");
        if (record.GameMode == "in-place" && !string.IsNullOrEmpty(record.GameDataRoot))
            foreach (var name in owned)
                if (PathBudget.IsInside(record.GameDataRoot, Path.Combine(layout.Root, name)))
                    throw new SetupException($"The game is kept inside {name}, which would be removed. Nothing was removed.");

        var removed = new List<string>();
        var kept = new List<string>();
        // install.json goes last: a removal that stops half way can be run again.
        foreach (var name in owned.OrderBy(n => string.Equals(n, InstallLayout.RecordName, StringComparison.OrdinalIgnoreCase) ? 1 : 0))
        {
            var path = Path.Combine(layout.Root, name);
            if (File.Exists(path)) { Retry.Io(() => File.Delete(path)); removed.Add(name); }
            else if (Directory.Exists(path)) { SafeFolder.Delete(path); removed.Add(name + "\\"); }
        }
        // What a save that was cut short leaves beside install.json.
        foreach (var temp in Directory.EnumerateFiles(layout.Root, InstallLayout.RecordName + ".*.tmp"))
        {
            File.Delete(temp);
            removed.Add(Path.GetFileName(temp));
        }
        if (record.GameMode == "in-place" && !string.IsNullOrEmpty(record.GameDataRoot))
            kept.Add($"{record.GameDataRoot} (the game, where it is)");
        // The pointer beside the exe: only the one that names this install.
        if (exeDirectory != null)
        {
            var pointer = InstallRecord.PointerFile(exeDirectory);
            if (InstallRecord.Load(pointer) is { } p && string.Equals(Path.GetFullPath(p.InstallRoot).TrimEnd('\\'), layout.Root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            {
                File.Delete(pointer);
                removed.Add(pointer);
            }
        }
        // The folder itself, if nothing is left in it (something the player put there stays, and so does the folder).
        if (Directory.Exists(layout.Root) && !Directory.EnumerateFileSystemEntries(layout.Root).Any())
        {
            Directory.Delete(layout.Root);
            removed.Add(layout.Root);
        }
        else if (Directory.Exists(layout.Root))
        {
            kept.Add($"{layout.Root} (it holds things the setup did not make)");
        }
        return new CleanupResult(removed, kept, $"{removed.Count} items removed; {kept.Count} kept");
    }

    /// Why this folder must never be removed as an install, or null.
    internal static string? Refusal(string root)
    {
        var full = Path.GetFullPath(root).TrimEnd('\\');
        if (Path.GetPathRoot(full)!.TrimEnd('\\').Equals(full, StringComparison.OrdinalIgnoreCase)) return "The install folder is a whole drive.";
        try
        {
            var info = new DirectoryInfo(full);
            if (info.Exists && info.Attributes.HasFlag(FileAttributes.ReparsePoint)) return "The install folder is a link.";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        var profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        var protectedFolders = new List<string>
        {
            profile,
            Environment.GetFolderPath(Environment.SpecialFolder.Windows),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
            Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
            Environment.GetFolderPath(Environment.SpecialFolder.Desktop),
            Path.Combine(profile, "Downloads"),
        };
        foreach (var folder in protectedFolders.Where(f => f.Length > 0))
        {
            var known = Path.GetFullPath(folder).TrimEnd('\\');
            // The folder itself, or one that holds it (C:\Users, C:\): removing its "tools" or "logs" would hit the user's own.
            if (string.Equals(known, full, StringComparison.OrdinalIgnoreCase) || PathBudget.IsInside(known, full))
                return "That folder is one of the user's or Windows' own folders, not a folder the setup made.";
        }
        return null;
    }

    static bool GameRunningFrom(string root)
    {
        foreach (var process in Process.GetProcessesByName("mcla"))
        {
            try
            {
                if (process.MainModule?.FileName is { } exe && PathBudget.IsInside(exe, root)) return true;
            }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException) { }
            finally { process.Dispose(); }
        }
        return false;
    }
}

// Removing an install: only what the setup created, in the install folder it recorded; never what it did
// not (the game's own folder when it was used where it is, the player's saves and shader cache, anything else
// in the install folder); never through a link.
namespace MclaLauncher.Setup;

sealed record CleanupResult(IReadOnlyList<string> Removed, IReadOnlyList<string> Kept, string Summary);

static class InstallCleanup
{
    // What the setup puts in the install folder, and nothing else: these are removed.
    static readonly string[] Created =
        [InstallLayout.RepoFolder, "tools", "downloads", "game-incoming", "logs", InstallLayout.RecordName];

    /// Removes the install (and the pointer beside the exe, if given). Refuses a folder that has no install.json of the
    /// setup's own, so that a wrong path never deletes a folder the setup didn't make.
    public static CleanupResult Uninstall(string root, string? exeDirectory)
    {
        var layout = new InstallLayout(Path.GetFullPath(root));
        var record = InstallRecord.Load(layout.RecordFile);
        if (record == null || !string.Equals(Path.GetFullPath(record.InstallRoot).TrimEnd('\\'), layout.Root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            throw new SetupException($"{layout.Root} is not an install made by the setup (it has no install.json of its own), so nothing was removed.");
        if (Path.GetPathRoot(layout.Root)!.TrimEnd('\\').Equals(layout.Root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            throw new SetupException("The install folder is a whole drive; nothing was removed.");

        var removed = new List<string>();
        var kept = new List<string>();
        foreach (var name in Created)
        {
            var path = Path.Combine(layout.Root, name);
            if (File.Exists(path)) { File.Delete(path); removed.Add(name); }
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
}

// The install the wizard makes: where everything is in it (InstallLayout), what is recorded about it
// (install.json), and the rules for the folder it goes in (PathBudget).
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MclaLauncher.Setup;

/// Where the parts of an install are, below the folder the player chose:
///   install.json    the record (also a pointer next to the launcher's exe)
///   tools\          pinned, checked downloads (llvm-mingw, CMake, Ninja, MinGit)
///   downloads\      what is being downloaded (the SDK bundle, kept until the build has used it)
///   game-incoming\  the game while it is copied, moved into mcla-recomp\game when complete
///   logs\           the setup's logs
///   kept\           the player's settings of an older checkout, while a newer release replaces it (put back after the build)
///   install.lock    held open while a setup runs here
///   mcla-recomp\    a git checkout of the fork at the release tag; the build's scripts, game\, generated\, out\
sealed record InstallLayout(string Root)
{
    public const string RepoFolder = "mcla-recomp";
    public const string RecordName = "install.json";
    public const string KeptFolder = "kept";
    /// What the setup makes directly in the install folder (what a removal may touch); recorded in install.json.
    public static readonly string[] OwnedNames = [RepoFolder, "tools", "downloads", "game-incoming", KeptFolder, "logs", InstallLock.FileName, RecordName];

    public string Repo => Path.Combine(Root, RepoFolder);
    public string Tools => Path.Combine(Root, "tools");
    public string Downloads => Path.Combine(Root, "downloads");
    public string Staging => Path.Combine(Root, "game-incoming");
    public string Kept => Path.Combine(Root, KeptFolder);
    public string LogsDir => Path.Combine(Root, "logs");
    public string RecordFile => Path.Combine(Root, RecordName);
    public string GameDir => Path.Combine(Repo, "game");
    public string LauncherDir => Path.Combine(Repo, "launcher");
    public string BuildScript => Path.Combine(LauncherDir, "build-game.ps1");
    public string BuildDir => Path.Combine(Repo, "out", "build", "win-amd64-release");
    public string GameExe => Path.Combine(BuildDir, "mcla.exe");

    /// The folder of MinGit's git.exe once it is unpacked (tools\mingit-*\cmd), or null.
    public string? GitDir
    {
        get
        {
            if (!Directory.Exists(Tools)) return null;
            foreach (var dir in new DirectoryInfo(Tools).EnumerateDirectories("mingit*"))
            {
                var cmd = Path.Combine(dir.FullName, "cmd");
                if (File.Exists(Path.Combine(cmd, "git.exe"))) return cmd;
            }
            return null;
        }
    }
}

/// What the wizard records about an install: where it is, which release it was made from, how far setup got,
/// and where the game is. Written next to the exe as a pointer (so the launcher finds the install) and in the
/// install folder.
sealed record InstallRecord
{
    public const int CurrentSchema = 1;

    public int Schema { get; init; } = CurrentSchema;
    public string InstallRoot { get; init; } = "";
    /// "setting-up" until the last step is done, then "ready".
    public string State { get; init; } = "setting-up";
    /// "release": follows the fork's release tags. (Phase 5 adds "main".)
    public string Channel { get; init; } = "release";
    public string Toolchain { get; init; } = "mingw";
    public string? ReleaseTag { get; init; }
    public string? ReleaseCommit { get; init; }
    /// "copy": the game was copied into the install; "in-place": it stays where it was, and game_data_root names it.
    public string GameMode { get; init; } = "copy";
    public string? GameSource { get; init; }
    public string? GameDataRoot { get; init; }
    public DateTime CreatedUtc { get; init; } = DateTime.UtcNow;
    public DateTime? FinishedUtc { get; init; }
    /// The steps that are done, by name (Game, Tools, Source, Build, Shaders).
    public List<string> StepsDone { get; init; } = [];
    /// The names in the install folder that this setup made: all a removal may delete there, nothing else.
    public List<string> Owned { get; init; } = [.. InstallLayout.OwnedNames];

    public bool Ready => State == "ready";

    /// Read without keeping anyone from replacing the file (the window looks at it while the setup saves it).
    public static InstallRecord? Load(string file)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                if (!File.Exists(file)) return null;
                using var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                var record = JsonSerializer.Deserialize(stream, InstallJson.Default.InstallRecord);
                return record is { InstallRoot.Length: > 0 } ? record : null;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException && attempt < 5)
            {
                Thread.Sleep(40);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
            {
                return null;
            }
        }
    }

    /// Written whole and renamed, so that a crash can't leave half a file. Another program with the file open
    /// (a virus scanner, the indexer) can make the rename fail for a moment: it is tried again.
    public void Save(string file)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(file)!);
        var temp = $"{file}.{Environment.ProcessId}.tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(this, InstallJson.Default.InstallRecord), new UTF8Encoding(false));
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                File.Move(temp, file, overwrite: true);
                return;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException && attempt < 40)
            {
                Thread.Sleep(50);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                try { File.Delete(temp); } catch (Exception inner) when (inner is IOException or UnauthorizedAccessException) { }
                throw;
            }
        }
    }

    /// The pointer beside the launcher's exe: just where the install is.
    public static string PointerFile(string exeDirectory) => Path.Combine(exeDirectory, InstallLayout.RecordName);
}

[JsonSourceGenerationOptions(WriteIndented = true, PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase,
                             DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull)]
[JsonSerializable(typeof(InstallRecord))]
partial class InstallJson : JsonSerializerContext;

/// The rules for the folder the player chooses. The numbers are what the install needs: the longest path
/// below the folder must stay under what Windows' programs accept (MAX_PATH, 259 characters, whatever the
/// long-path setting says: PowerShell 5.1 and Explorer have it, so does anything the build runs), and
/// scripts\make_pc.ps1 itself refuses a checkout folder over 148.
static class PathBudget
{
    /// Windows' MAX_PATH, without the terminating NUL.
    public const int MaxPath = 259;
    /// make_pc.ps1: "this folder's path is N characters long, and git cannot clone the SDK into one over 148".
    public const int MakePcRepoLimit = 148;
    /// The longest path below the install folder, from the folder, plus a little. Measured on finished installs
    /// (the check harness's `install` prints it and fails if it is above this): 127, in CMake's own documentation
    /// (tools\cmake-...\doc\cmake\html\_sources\variable\...); the longest file the build reads is 124
    /// (mcla-recomp\third_party\rexglue-sdk-bin\include\rex\ui\vulkan\functions\...). The compiler's object files
    /// are longer in a short folder (131, for the SDK's windowed_app_main_sdl.cpp below ...\CMakeFiles\mcla.dir\)
    /// but CMake moves one into a short hashed folder when its path would pass 250 characters, so they never
    /// decide. A new tool version can change the 127: run the harness's `install` when tools.json changes.
    public const int LongestBelowRoot = 129;

    /// How many characters the install folder's own path may have.
    public static int MaxRootLength =>
        Math.Min(MaxPath - 1 - LongestBelowRoot,                                     // "\" + the longest path below it
                 MakePcRepoLimit - (InstallLayout.RepoFolder.Length + 1));           // make_pc.ps1 looks at ...\mcla-recomp

    /// What is wrong with a folder as an install folder, in plain words; null when it is fine.
    public static string? Problem(string path, out bool resumes)
    {
        resumes = false;
        if (string.IsNullOrWhiteSpace(path)) return "Choose a folder for the game and its tools.";
        string full;
        try { full = Path.GetFullPath(path.Trim()); }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return "That isn't a folder name Windows accepts.";
        }
        if (!Path.IsPathRooted(path.Trim()) || full.StartsWith(@"\\", StringComparison.Ordinal))
            return "Choose a folder on a drive of this PC (a network location won't do: the build writes thousands of small files).";
        var root = Path.GetPathRoot(full)!;
        if (UnsafeCharacter(full) is { } bad)
            return $"The path has a character the build can't take ({bad}): its tools read it as a plain ANSI string, and Windows' command line gives some characters a meaning of their own. "
                 + "Use only letters without accents, digits, spaces, dots, dashes and underscores in the whole path, for example C:\\MCLA.";
        if (DriveProblem(root) is { } drive) return drive;
        if (string.Equals(full.TrimEnd('\\'), root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            return "Choose a folder, not the whole drive.";
        if (full.TrimEnd('\\').Length > MaxRootLength)
            return $"That path is {full.TrimEnd('\\').Length} characters long, and the build needs it to stay at {MaxRootLength} or fewer: " +
                   "some of the files it makes have long names, and Windows' tools stop at 260 characters in all. Choose a shorter path, for example C:\\MCLA.";
        var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        var programs = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        var programsX86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        foreach (var protectedFolder in new[] { windows, programs, programsX86 })
            if (protectedFolder.Length > 0 && IsInside(full, protectedFolder))
                return "Windows doesn't let programs write there without administrator rights. Choose a folder in your own space, for example C:\\MCLA.";
        if (Directory.Exists(full))
        {
            if (InstallRecord.Load(Path.Combine(full, InstallLayout.RecordName)) is { InstallRoot.Length: > 0 })
            {
                if (InstallLock.IsHeld(full)) return "A setup is running in that folder right now (in another window of this program). Wait for it to finish, or choose another folder.";
                resumes = true;
                return null;
            }
            bool empty;
            try { empty = !Directory.EnumerateFileSystemEntries(full).Any(); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return $"That folder can't be read: {ex.Message}"; }
            if (!empty)
                return "That folder already has other things in it. Choose a new or empty folder: the setup puts its files there and nothing else.";
        }
        return null;
    }

    /// The first character of the path (after the drive) that the build can't take, shown for the message; null when all are fine.
    /// Allowed: A-Z a-z 0-9, space, dot, dash, underscore and the separators. The SDK's code generator opens its files by
    /// a path that is not Unicode (an accented folder fails with "File could not be opened for reading"), and the commands
    /// CMake and Ninja run through cmd.exe break at & ^ % ! and the like; a semicolon would split a PATH entry.
    public static string? UnsafeCharacter(string path)
    {
        var full = Path.GetFullPath(path.Trim());
        var rest = Path.GetPathRoot(full) is { } r ? full[r.Length..] : full;
        foreach (var c in rest)
            if (!(c is >= 'A' and <= 'Z' || c is >= 'a' and <= 'z' || c is >= '0' and <= '9' || c is ' ' or '.' or '_' or '-' or '\\'))
                return c < ' ' ? $"U+{(int)c:X4}" : c.ToString();
        return null;
    }

    /// The build and git need a local NTFS (or ReFS) drive: a mapped network drive is as slow as a share, and exFAT and
    /// FAT32 have no owners, which Git for Windows refuses ("dubious ownership"). Null when the drive is fine or can't be told.
    static string? DriveProblem(string root)
    {
        try
        {
            var drive = new DriveInfo(root);
            if (drive.DriveType == DriveType.Network)
                return "Choose a folder on a drive of this PC (a mapped network drive won't do: the build writes thousands of small files).";
            if (!drive.IsReady) return $"The drive {root.TrimEnd('\\')} isn't ready.";
            if (!drive.DriveFormat.Equals("NTFS", StringComparison.OrdinalIgnoreCase) && !drive.DriveFormat.Equals("ReFS", StringComparison.OrdinalIgnoreCase))
                return $"The drive {root.TrimEnd('\\')} is formatted as {drive.DriveFormat}. The build needs an NTFS drive (Git refuses folders on other formats). Choose a folder on another drive, for example C:\\MCLA.";
        }
        catch (Exception ex) when (ex is ArgumentException or IOException or UnauthorizedAccessException) { }
        return null;
    }

    /// A warning that doesn't stop the setup: OneDrive (or another cloud folder) makes thousands of small
    /// files slow and can lock them while it syncs.
    public static string? Warning(string path)
    {
        try
        {
            var full = Path.GetFullPath(path.Trim());
            foreach (var name in new[] { "OneDrive", "OneDriveConsumer", "OneDriveCommercial" })
                if (Environment.GetEnvironmentVariable(name) is { Length: > 0 } onedrive && IsInside(full, onedrive))
                    return "This folder is inside OneDrive. OneDrive would sync the game's 8 GB and the thousands of small files the build writes, which makes it slow and can make it fail. A folder outside OneDrive is better, for example C:\\MCLA.";
            if (full.Split('\\').Any(part => part.StartsWith("OneDrive", StringComparison.OrdinalIgnoreCase)))
                return "This folder looks like it is inside OneDrive. OneDrive would sync the game's 8 GB and the thousands of small files the build writes. A folder outside OneDrive is better, for example C:\\MCLA.";
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException) { }
        return null;
    }

    public static bool IsInside(string path, string folder)
    {
        var a = Path.GetFullPath(path).TrimEnd('\\') + "\\";
        var b = Path.GetFullPath(folder).TrimEnd('\\') + "\\";
        return a.StartsWith(b, StringComparison.OrdinalIgnoreCase);
    }

    /// Free bytes on the drive the folder is (or will be) on.
    public static long FreeBytes(string path)
    {
        try
        {
            var root = Path.GetPathRoot(Path.GetFullPath(path.Trim()));
            return root == null ? 0 : new DriveInfo(root).AvailableFreeSpace;
        }
        catch (Exception ex) when (ex is ArgumentException or IOException or UnauthorizedAccessException) { return 0; }
    }

    /// What an install needs besides the disc image: the game (copied), the tools unpacked, the checkout, the
    /// SDK, the build and the shader storage, with room to spare while the downloads and the unpacked tools exist.
    public static long RequiredBytes(long gameBytes, bool copiesGame) => (copiesGame ? gameBytes : 0) + 3L * (1L << 30);

    /// What a setup in a folder that already holds part of it still needs: the game's bytes not yet in game-incoming\
    /// or the checkout's game\ (when it is copied), 1.5 GB for the tools until they are unpacked, and 1.5 GB for the
    /// checkout, the build and the shaders until the game is built. A fresh folder comes to RequiredBytes.
    public static long RemainingBytes(InstallLayout layout, InstallRecord record, long gameBytes, bool copiesGame, bool toolsDone, bool built)
    {
        const long Gb15 = 3L << 29;
        long game = 0;
        if (copiesGame && !record.StepsDone.Contains("Game"))
            game = Math.Max(0, gameBytes - Math.Min(gameBytes, DirectoryBytes(layout.Staging) + DirectoryBytes(layout.GameDir)));
        return game + (toolsDone ? 0 : Gb15) + (built ? 0 : Gb15);
    }

    /// The size of the files in a folder, not following links (0 when it isn't there).
    public static long DirectoryBytes(string folder)
    {
        long sum = 0;
        try
        {
            if (!Directory.Exists(folder)) return 0;
            var options = new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true, AttributesToSkip = FileAttributes.ReparsePoint };
            foreach (var file in new DirectoryInfo(folder).EnumerateFiles("*", options)) sum += file.Length;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        return sum;
    }
}

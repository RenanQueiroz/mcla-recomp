// What the setup wizard knows about the supported game, and the check of a disc image or a folder against
// it: the fingerprints (names, sizes and SHA-256 of the files, the title id and version in default.xex)
// come from the release (release.json's "game"; scripts/ci/game-files.json makes them), and the verdict
// is said in plain words. Nothing here reads more than a header and the file system: the files themselves
// are hashed while they are copied (GameCopier.cs).
using System.Buffers.Binary;
using System.Security.Cryptography;

namespace MclaLauncher.Setup;

sealed record GameFileInfo(string Path, long Size, string Sha256);

sealed record GameFingerprint(string Title, string TitleId, string XexVersion, IReadOnlyList<GameFileInfo> Files)
{
    public const string XexName = "default.xex";
    public long TotalSize => Files.Sum(f => f.Size);
    public GameFileInfo Xex => Files.First(f => string.Equals(f.Path, XexName, StringComparison.OrdinalIgnoreCase));
}

/// The title id and version in a default.xex header.
sealed record XexInfo(uint TitleId, uint Version)
{
    public string TitleIdText => TitleId.ToString("X8");
    /// 4 bits major, 4 minor, 16 build, 8 revision, as the console's tools print it.
    public string VersionText => $"{Version >> 28}.{(Version >> 24) & 0xF}.{(Version >> 8) & 0xFFFF}.{Version & 0xFF}";

    /// Reads the execution info of an XEX2 header (the first 16 KiB of the file are enough); null when it isn't one.
    public static XexInfo? Read(ReadOnlySpan<byte> head)
    {
        if (head.Length < 0x18 || !head[..4].SequenceEqual("XEX2"u8)) return null;
        uint count = BinaryPrimitives.ReadUInt32BigEndian(head[0x14..]);
        for (uint i = 0; i < count && 0x18 + (i + 1) * 8 <= head.Length; i++)
        {
            uint key = BinaryPrimitives.ReadUInt32BigEndian(head[(int)(0x18 + i * 8)..]);
            uint value = BinaryPrimitives.ReadUInt32BigEndian(head[(int)(0x1C + i * 8)..]);
            if (key != 0x00040006) continue;
            if (value + 16 > head.Length) return null;
            return new XexInfo(BinaryPrimitives.ReadUInt32BigEndian(head[(int)(value + 12)..]),
                               BinaryPrimitives.ReadUInt32BigEndian(head[(int)(value + 4)..]));
        }
        return null;
    }
}

/// Where the game's files are: a disc image, or a folder.
abstract class GameSource : IDisposable
{
    /// The files by their path from the root, in the order of their names ("default.xex", "nxeart", ...).
    public abstract IReadOnlyList<SourceFile> Files { get; }
    public abstract Stream Open(SourceFile file);
    public abstract string Describe { get; }
    public virtual void Dispose() { }
    public bool IsFolder => this is FolderSource;

    public static GameSource Open(string path)
    {
        if (Directory.Exists(path)) return new FolderSource(path);
        if (File.Exists(path)) return new DiscSource(path);
        throw new FileNotFoundException("That file or folder isn't there.");
    }
}

sealed record SourceFile(string Path, long Size, object? Handle = null);

sealed class DiscSource : GameSource
{
    readonly XboxDiscImage image;
    readonly List<SourceFile> files;
    public DiscSource(string path)
    {
        image = XboxDiscImage.Open(path);
        files = image.Entries.Select(e => new SourceFile(e.Path.TrimStart('/'), e.Size, e)).ToList();
        Describe = System.IO.Path.GetFileName(path);
    }
    public override IReadOnlyList<SourceFile> Files => files;
    public override string Describe { get; }
    public override Stream Open(SourceFile file) => image.OpenFile((DiscEntry)file.Handle!);
    public override void Dispose() => image.Dispose();
}

sealed class FolderSource : GameSource
{
    readonly string root;
    readonly List<SourceFile> files = [];
    public FolderSource(string path)
    {
        root = System.IO.Path.GetFullPath(path);
        Describe = root;
        // The game's files all live in the folder itself (a disc has no folders of its own but the console's
        // update); the folder's subfolders are not read, so that a wrong choice such as a whole drive costs
        // nothing, and files that are not the game's are left alone.
        var options = new EnumerationOptions { RecurseSubdirectories = false, IgnoreInaccessible = true, AttributesToSkip = FileAttributes.ReparsePoint };
        foreach (var file in new DirectoryInfo(root).EnumerateFiles("*", options))
        {
            var relative = System.IO.Path.GetRelativePath(root, file.FullName).Replace('\\', '/');
            files.Add(new SourceFile(relative, file.Length, file.FullName));
        }
        files.Sort((a, b) => string.CompareOrdinal(a.Path, b.Path));
    }
    public override IReadOnlyList<SourceFile> Files => files;
    public override string Describe { get; }
    public override Stream Open(SourceFile file) =>
        new FileStream((string)file.Handle!, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.SequentialScan);
}

enum GameVerdict
{
    Ok,
    /// Not a disc image or a folder with the game in it at all.
    NotAGame,
    /// A different Xbox 360 game (another title id).
    AnotherGame,
    /// Midnight Club: Los Angeles, but not the release this build is for.
    AnotherRelease,
    /// The right release with files missing or the wrong size.
    DamagedDump,
}

sealed record FileCheck(string Path, long Expected, long? Found)
{
    public bool Present => Found != null;
    public bool Ok => Found == Expected;
}

sealed record GameCheckResult(GameVerdict Verdict, string Headline, string Detail, IReadOnlyList<FileCheck> Files, string? TitleId = null, string? XexVersion = null)
{
    public bool Ok => Verdict == GameVerdict.Ok;
}

static class GameChecker
{
    /// Opens the image or the folder and checks it against the fingerprint: the file system, default.xex
    /// (title id, version, SHA-256) and the names and sizes of the files. Throws nothing for a game that
    /// isn't the right one: that is a verdict.
    public static GameCheckResult Inspect(string path, GameFingerprint expected, out GameSource? source)
    {
        source = null;
        try
        {
            source = GameSource.Open(path);
        }
        catch (NotAnXboxDiscException ex)
        {
            return NotAGame($"{System.IO.Path.GetFileName(path)} doesn't look like an Xbox 360 disc image. {ex.Message} "
                            + "Choose the .iso of your Midnight Club: Los Angeles disc, or the folder it was extracted into.");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or FileNotFoundException)
        {
            return NotAGame($"That can't be read: {ex.Message}");
        }

        var result = Inspect(source, expected);
        if (!result.Ok)
        {
            source.Dispose();
            source = null;
        }
        return result;
    }

    public static GameCheckResult Inspect(GameSource source, GameFingerprint expected)
    {
        var byName = source.Files.GroupBy(f => f.Path, StringComparer.OrdinalIgnoreCase).ToDictionary(g => g.Key, g => g.First(), StringComparer.OrdinalIgnoreCase);
        var checks = expected.Files.Select(f => new FileCheck(f.Path, f.Size, byName.TryGetValue(f.Path, out var found) ? found.Size : null)).ToList();

        if (!byName.TryGetValue(GameFingerprint.XexName, out var xexFile))
        {
            return NotAGame(source.IsFolder
                ? $"There is no {GameFingerprint.XexName} in that folder, so it isn't an extracted Midnight Club: Los Angeles. Choose the folder that holds {GameFingerprint.XexName} itself, or the .iso."
                : $"That disc image has no {GameFingerprint.XexName}, so it isn't a game disc. Choose the .iso of your Midnight Club: Los Angeles disc.", checks);
        }

        // default.xex: its header says which game and which release it is, and its hash whether it is the one supported.
        byte[] head;
        string xexHash;
        try
        {
            using var stream = source.Open(xexFile);
            using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            var buffer = new byte[1 << 20];
            var headBuffer = new byte[0x4000];
            int headLength = 0, read;
            while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
            {
                sha.AppendData(buffer, 0, read);
                if (headLength < headBuffer.Length)
                {
                    int take = Math.Min(read, headBuffer.Length - headLength);
                    Array.Copy(buffer, 0, headBuffer, headLength, take);
                    headLength += take;
                }
            }
            head = headBuffer[..headLength];
            xexHash = Convert.ToHexStringLower(sha.GetHashAndReset());
        }
        catch (Exception ex) when (ex is IOException or NotAnXboxDiscException or UnauthorizedAccessException)
        {
            return NotAGame($"{GameFingerprint.XexName} can't be read: {ex.Message}", checks);
        }

        var info = XexInfo.Read(head);
        if (info == null)
        {
            return NotAGame($"{GameFingerprint.XexName} isn't an Xbox 360 program (it doesn't start with the XEX2 mark), so this isn't a game dump.", checks);
        }
        if (!string.Equals(info.TitleIdText, expected.TitleId, StringComparison.OrdinalIgnoreCase))
        {
            return new GameCheckResult(GameVerdict.AnotherGame, "This is another game",
                $"Its title id is {info.TitleIdText}; Midnight Club: Los Angeles is {expected.TitleId}. This installer only builds Midnight Club: Los Angeles.",
                checks, info.TitleIdText, info.VersionText);
        }
        if (info.VersionText != expected.XexVersion)
        {
            return new GameCheckResult(GameVerdict.AnotherRelease, "This is another release of Midnight Club: Los Angeles",
                $"Its program is version {info.VersionText}; this installer supports the {expected.Title}, version {expected.XexVersion}. "
                + "A different region's disc or a title update has a different program, and the recompiled game would not match it.",
                checks, info.TitleIdText, info.VersionText);
        }
        if (!string.Equals(xexHash, expected.Xex.Sha256, StringComparison.OrdinalIgnoreCase))
        {
            // The same version number with other contents: the program was changed, or the dump is damaged.
            return new GameCheckResult(GameVerdict.DamagedDump, "A damaged or modified default.xex",
                $"Its program says it is the right version ({info.VersionText}), but it is not the original: its contents differ from the {expected.Title}'s. "
                + "Dump the disc again, or extract default.xex again.",
                checks, info.TitleIdText, info.VersionText);
        }

        var missing = checks.Where(c => !c.Present).Select(c => c.Path).ToList();
        var wrongSize = checks.Where(c => c.Present && !c.Ok).Select(c => c.Path).ToList();
        if (missing.Count > 0 || wrongSize.Count > 0)
        {
            var parts = new List<string>();
            if (missing.Count > 0) parts.Add($"missing: {string.Join(", ", missing)}");
            if (wrongSize.Count > 0) parts.Add($"the wrong size: {string.Join(", ", wrongSize)}");
            return new GameCheckResult(GameVerdict.DamagedDump, "A damaged or incomplete dump",
                $"The program is the right one, but {string.Join("; ", parts)}. Dump the disc again, or extract all of it.",
                checks, info.TitleIdText, info.VersionText);
        }

        return new GameCheckResult(GameVerdict.Ok, expected.Title,
            $"All {expected.Files.Count} files are there ({FormatSize(expected.TotalSize)}). Each one is checked again while it is copied.",
            checks, info.TitleIdText, info.VersionText);
    }

    static GameCheckResult NotAGame(string detail, IReadOnlyList<FileCheck>? files = null) =>
        new(GameVerdict.NotAGame, "This isn't a Midnight Club: Los Angeles disc or folder", detail, files ?? []);

    public static string FormatSize(long bytes) =>
        bytes >= 1L << 30 ? $"{bytes / (double)(1L << 30):0.0} GB" : $"{bytes / (double)(1L << 20):0} MB";
}

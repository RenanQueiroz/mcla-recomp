// Copying the game out of a disc image (or a folder) into the install, hashing every file as it is written:
// a bad dump is found here at no cost of its own. Written to a staging folder and moved into place when
// complete; files already copied and checked are kept, so a cancelled or failed copy resumes where it was.
using System.Security.Cryptography;
using System.Text.Json;

namespace MclaLauncher.Setup;

/// Something wrong with a file of the game's, in plain words: a damaged dump, or a disk that is full or won't be written.
sealed class GameFileException(string message, string? detail = null) : SetupException(message, detail);

sealed record CopyProgress(long Done, long Total, string File);

static class GameCopier
{
    /// The ledger of files copied and checked in the staging folder: name, size, SHA-256.
    const string LedgerName = ".mcla-copied.json";

    /// Copies the files of the fingerprint from the source into `destination` (a staging folder), each checked
    /// against its SHA-256 as it goes, and writes the ledger. Files the ledger lists with the size on disk are kept.
    public static async Task CopyAsync(GameSource source, GameFingerprint expected, string destination,
                                       IProgress<CopyProgress> progress, CancellationToken ct)
    {
        Directory.CreateDirectory(destination);
        var ledger = ReadLedger(destination);
        long total = expected.TotalSize, done = 0;
        var buffer = new byte[4 << 20];
        var bySource = source.Files.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
        foreach (var file in expected.Files)
        {
            ct.ThrowIfCancellationRequested();
            var target = Path.Combine(destination, file.Path);
            if (ledger.TryGetValue(file.Path, out var known) && known == file.Sha256
                && File.Exists(target) && new FileInfo(target).Length == file.Size)
            {
                done += file.Size;
                progress.Report(new CopyProgress(done, total, file.Path));
                continue;
            }
            if (!bySource.TryGetValue(file.Path, out var from))
            {
                if (file.Optional) { done += file.Size; continue; }
                throw new GameFileException($"{file.Path} is missing from your game.", "The game isn't complete.");
            }
            var partial = target + ".part";
            using (var input = source.Open(from))
            using (var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
            {
                long fileDone = 0;
                try
                {
                    await using var output = new FileStream(partial, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, FileOptions.SequentialScan);
                    int read;
                    while ((read = await input.ReadAsync(buffer, ct)) > 0)
                    {
                        sha.AppendData(buffer, 0, read);
                        await output.WriteAsync(buffer.AsMemory(0, read), ct);
                        fileDone += read;
                        progress.Report(new CopyProgress(done + fileDone, total, file.Path));
                    }
                }
                catch (IOException ex) when (!(ex is FileNotFoundException))
                {
                    var driveFull = ex.HResult == unchecked((int)0x80070070) || ex.HResult == unchecked((int)0x80070027);
                    throw new GameFileException(driveFull ? "The disk is full. Free some space and try again." : $"Couldn't copy {file.Path}: {ex.Message}", ex.ToString());
                }
                if (fileDone != file.Size)
                {
                    TryDelete(partial);
                    throw new GameFileException($"{file.Path} has the wrong size: your game is damaged or incomplete.",
                                                $"expected {file.Size} bytes, read {fileDone}");
                }
                var hash = Convert.ToHexStringLower(sha.GetHashAndReset());
                if (hash != file.Sha256)
                {
                    TryDelete(partial);
                    throw new GameFileException($"{file.Path} is damaged: its contents are not the ones on the original disc. Dump the disc again, or check where the copy came from.",
                                                $"expected sha256 {file.Sha256}, got {hash}");
                }
            }
            Retry.Io(() => File.Move(partial, target, overwrite: true));
            ledger[file.Path] = file.Sha256;
            WriteLedger(destination, ledger);
            done += file.Size;
            progress.Report(new CopyProgress(done, total, file.Path));
        }
    }

    /// Reads every file of the fingerprint from the source and compares its SHA-256, writing nothing (the game
    /// stays where it is: the check that copying would have made).
    public static async Task VerifyAsync(GameSource source, GameFingerprint expected, IProgress<CopyProgress> progress, CancellationToken ct)
    {
        long total = expected.TotalSize, done = 0;
        var buffer = new byte[4 << 20];
        var bySource = source.Files.ToDictionary(f => f.Path, StringComparer.OrdinalIgnoreCase);
        foreach (var file in expected.Files)
        {
            ct.ThrowIfCancellationRequested();
            if (!bySource.TryGetValue(file.Path, out var from))
            {
                if (file.Optional) { done += file.Size; continue; }
                throw new GameFileException($"{file.Path} is missing from your game.");
            }
            using var input = source.Open(from);
            using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            long fileDone = 0;
            int read;
            while ((read = await input.ReadAsync(buffer, ct)) > 0)
            {
                sha.AppendData(buffer, 0, read);
                fileDone += read;
                progress.Report(new CopyProgress(done + fileDone, total, file.Path));
            }
            var hash = Convert.ToHexStringLower(sha.GetHashAndReset());
            if (fileDone != file.Size || hash != file.Sha256)
                throw new GameFileException($"{file.Path} is damaged: its contents are not the ones on the original disc. Dump the disc again, or check where the copy came from.",
                                            $"expected {file.Size} bytes sha256 {file.Sha256}; got {fileDone} bytes sha256 {hash}");
            done += file.Size;
        }
    }

    /// Moves the finished staging folder to its place: the game folder of the checkout. The ledger stays behind
    /// (it is the staging folder's). Fails, leaving both as they were, if the place has files already.
    public static void MoveIntoPlace(string staging, string gameDir, GameFingerprint expected)
    {
        Directory.CreateDirectory(gameDir);
        foreach (var file in expected.Files)
        {
            var from = Path.Combine(staging, file.Path);
            var to = Path.Combine(gameDir, file.Path);
            if (File.Exists(to) && new FileInfo(to).Length == file.Size) { Retry.Io(() => File.Delete(from)); continue; }
            if (!File.Exists(from) && file.Optional) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(to)!);
            Retry.Io(() => File.Move(from, to, overwrite: true));
        }
        TryDeleteTree(staging);
    }

    /// All the game's files are in the folder with the sizes the fingerprint says (what a finished copy leaves).
    public static bool IsComplete(string gameDir, GameFingerprint expected) =>
        expected.Files.All(f =>
        {
            var path = Path.Combine(gameDir, f.Path);
            return File.Exists(path) ? new FileInfo(path).Length == f.Size : f.Optional;
        });

    static Dictionary<string, string> ReadLedger(string destination)
    {
        try
        {
            var file = Path.Combine(destination, LedgerName);
            return File.Exists(file)
                ? JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(file)) ?? []
                : [];
        }
        catch (Exception ex) when (ex is IOException or JsonException) { return []; }
    }

    static void WriteLedger(string destination, Dictionary<string, string> ledger)
    {
        var file = Path.Combine(destination, LedgerName);
        File.WriteAllText(file + ".tmp", JsonSerializer.Serialize(ledger));
        Retry.Io(() => File.Move(file + ".tmp", file, overwrite: true));
    }

    static void TryDelete(string file)
    {
        try { File.Delete(file); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }

    /// Deletes a folder this setup made, without following links: reparse points are removed as links, never entered.
    public static void TryDeleteTree(string folder)
    {
        try { SafeFolder.Delete(folder); } catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }
}

/// Deleting what the setup created. Never follows a reparse point (a junction or symbolic link inside the folder
/// is removed as the link it is; what it points to is left alone).
static class SafeFolder
{
    public static void Delete(string folder) => Retry.Io(() => DeleteOnce(folder));

    static void DeleteOnce(string folder)
    {
        var info = new DirectoryInfo(folder);
        if (!info.Exists) return;
        if (info.Attributes.HasFlag(FileAttributes.ReparsePoint))
        {
            info.Delete();   // the link itself, not what it points at
            return;
        }
        foreach (var entry in info.EnumerateFileSystemInfos())
        {
            if (entry.Attributes.HasFlag(FileAttributes.ReparsePoint))
            {
                if (entry is DirectoryInfo link) link.Delete(); else entry.Delete();
            }
            else if (entry is DirectoryInfo sub)
            {
                DeleteOnce(sub.FullName);
            }
            else
            {
                if (entry.Attributes.HasFlag(FileAttributes.ReadOnly)) entry.Attributes = FileAttributes.Normal;
                entry.Delete();
            }
        }
        info.Delete();
    }
}

// Getting a file from the release and trusting it only once its SHA-256 and size are the release's; unpacking
// the tool archives into the install's tools\ folder. Nothing downloaded is used before it is checked, and
// a file that doesn't match is deleted.
using System.IO.Compression;
using System.Security.Cryptography;

namespace MclaLauncher.Setup;

static class Verified
{
    public static async Task<string> Sha256Async(string file, CancellationToken ct)
    {
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        await using var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.SequentialScan);
        var buffer = new byte[1 << 20];
        int read;
        while ((read = await stream.ReadAsync(buffer, ct)) > 0) sha.AppendData(buffer, 0, read);
        return Convert.ToHexStringLower(sha.GetHashAndReset());
    }

    /// The same, without async: for a caller that has a UI thread to keep free of nothing but the 9 MB it reads.
    public static bool Is(string file, long size, string sha256)
    {
        if (!File.Exists(file) || new FileInfo(file).Length != size) return false;
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        using var stream = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.SequentialScan);
        var buffer = new byte[1 << 20];
        int read;
        while ((read = stream.Read(buffer, 0, buffer.Length)) > 0) sha.AppendData(buffer, 0, read);
        return Convert.ToHexStringLower(sha.GetHashAndReset()) == sha256.ToLowerInvariant();
    }

    /// True when the file is there with exactly this size and SHA-256.
    public static async Task<bool> IsAsync(string file, long size, string sha256, CancellationToken ct) =>
        File.Exists(file) && new FileInfo(file).Length == size && await Sha256Async(file, ct) == sha256.ToLowerInvariant();

    /// Gets `name` from `fetch` into `destination` (through a .part file), checks size and SHA-256 and moves it in place.
    /// A download that doesn't match is deleted and reported; up to three tries for a failed transfer.
    public static async Task FetchAsync(string name, string destination, long size, string sha256,
                                        Func<string, IProgress<long>, CancellationToken, Task> fetch,
                                        IProgress<long> progress, Action<string> log, CancellationToken ct)
    {
        if (await IsAsync(destination, size, sha256, ct))
        {
            log($"{name}: already downloaded and checked");
            progress.Report(size);
            return;
        }
        var partial = destination + ".part";
        for (int attempt = 1; ; attempt++)
        {
            try
            {
                await fetch(partial, progress, ct);
                var length = new FileInfo(partial).Length;
                if (length != size)
                    throw new TransferException($"{name} came to {length} bytes, not the {size} the release says");
                var actual = await Sha256Async(partial, ct);
                if (actual != sha256.ToLowerInvariant())
                {
                    File.Delete(partial);
                    throw new SetupException($"{name} doesn't match the release's checksum, so it was thrown away and not used. Try again; if it keeps happening, the download is being altered on the way.",
                                             $"expected sha256 {sha256}, got {actual}");
                }
                Retry.Io(() => File.Move(partial, destination, overwrite: true));
                log($"{name}: downloaded and checked ({size:N0} bytes)");
                return;
            }
            catch (Exception ex) when (ex is TransferException or SetupException { InnerException: HttpRequestException } or IOException && attempt < 3 && !ct.IsCancellationRequested)
            {
                log($"{name}: attempt {attempt} failed ({ex.Message}); trying again");
                await Task.Delay(TimeSpan.FromSeconds(2 * attempt), ct);
            }
        }
    }

    sealed class TransferException(string message) : Exception(message);
}

static class ToolInstaller
{
    const string Marker = ".mcla-checked";
    /// tools\.mcla-tools.json's neighbour: one line per unpacked tool, "name<TAB>folder<TAB>sha256 of its zip".
    const string RecordName = ".mcla-tools.txt";
    /// What an archive may hold: more is not a tool of this size (llvm-mingw is about 5,500 files and 900 MB unpacked).
    const long MaxUnpackedBytes = 4L << 30;
    const int MaxEntries = 100_000;

    /// The tool was unpacked from the zip with this SHA-256 and is still there (its marker says so): nothing to
    /// download or unpack for it again.
    public static bool IsInstalled(string tools, string name, string sha256)
    {
        try
        {
            var file = Path.Combine(tools, RecordName);
            if (!File.Exists(file)) return false;
            foreach (var line in File.ReadAllLines(file))
            {
                var parts = line.Split('\t');
                if (parts.Length != 3 || parts[0] != name || !string.Equals(parts[2], sha256, StringComparison.OrdinalIgnoreCase)) continue;
                var marker = Path.Combine(tools, parts[1], Marker);
                return File.Exists(marker) && File.ReadAllText(marker).Trim() == parts[2];
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException) { }
        return false;
    }

    static void Record(string tools, string name, string folder, string sha256)
    {
        var file = Path.Combine(tools, RecordName);
        var lines = File.Exists(file) ? File.ReadAllLines(file).Where(l => !l.StartsWith(name + "\t", StringComparison.Ordinal)).ToList() : [];
        lines.Add($"{name}\t{folder}\t{sha256}");
        Retry.Io(() => File.WriteAllLines(file, lines));
    }

    static readonly HashSet<string> DeviceNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };

    /// A name in an archive that Windows would treat as something other than a plain file or folder: an alternate
    /// data stream (a colon), a device (CON, NUL, COM1...), a trailing dot or space (dropped by Windows, so two names
    /// could become one), or a control character.
    internal static bool IsOddName(string entryName)
    {
        if (entryName.Contains(':') || entryName.Any(c => c < ' ' || c is '<' or '>' or '"' or '|' or '?' or '*')) return true;
        foreach (var segment in entryName.Replace('\\', '/').Split('/'))
        {
            if (segment.Length == 0) continue;
            if (segment.EndsWith('.') && segment != "." && segment != ".." || segment.EndsWith(' ')) return true;
            var stem = segment.Contains('.') ? segment[..segment.IndexOf('.')] : segment;
            if (DeviceNames.Contains(stem.TrimEnd())) return true;
        }
        return false;
    }

    /// Unpacks a tool's zip into tools\, next to its final place and moved there only when complete, so that an
    /// interrupted run leaves no half-unpacked tool that looks installed. A zip with one top folder (llvm-mingw,
    /// CMake) keeps its name; one without (Ninja, MinGit) goes in tools\<name>-<version>. The marker holds the zip's
    /// SHA-256: a tool already unpacked from this zip is left alone. Returns the folder.
    public static async Task<string> InstallAsync(string tools, string name, string version, string zipFile, string sha256,
                                                  Action<string> log, IProgress<(int Done, int Total)> progress, CancellationToken ct)
    {
        Directory.CreateDirectory(tools);
        string topFolder;
        bool singleTop;
        int total;
        using (var zip = ZipFile.OpenRead(zipFile))
        {
            var top = zip.Entries.Select(e => e.FullName.Replace('\\', '/').Split('/')[0]).Distinct().ToList();
            var hasLoose = zip.Entries.Any(e => !e.FullName.Replace('\\', '/').Contains('/') && e.Name.Length > 0);
            singleTop = top.Count == 1 && !hasLoose;
            topFolder = singleTop ? top[0] : $"{name}-{version}";
            total = zip.Entries.Count;
            if (total > MaxEntries || zip.Entries.Sum(e => e.Length) > MaxUnpackedBytes)
                throw new SetupException($"{name}'s archive is far bigger than a tool should be; it was not unpacked.", $"{total} entries, {zip.Entries.Sum(e => e.Length)} bytes");
        }
        if (topFolder.Contains("..") || Path.IsPathRooted(topFolder) || topFolder.Length == 0)
            throw new SetupException($"{name}'s archive has a name that can't be used.");
        var final = Path.Combine(tools, topFolder);
        var marker = Path.Combine(final, Marker);
        if (File.Exists(marker) && File.ReadAllText(marker).Trim() == sha256)
        {
            log($"{name}: already unpacked");
            progress.Report((total, total));
            Record(tools, name, topFolder, sha256);
            return final;
        }

        var temp = Path.Combine(tools, $".u-{name}");
        SafeFolder.Delete(temp);
        Directory.CreateDirectory(temp);
        var wrapped = !singleTop;
        var target = wrapped ? Path.Combine(temp, topFolder) : temp;
        Directory.CreateDirectory(target);
        var fullTarget = Path.GetFullPath(target).TrimEnd('\\') + "\\";
        int done = 0;
        await Task.Run(() =>
        {
            using var zip = ZipFile.OpenRead(zipFile);
            foreach (var entry in zip.Entries)
            {
                ct.ThrowIfCancellationRequested();
                if (IsOddName(entry.FullName))
                    throw new SetupException($"{name}'s archive has a file name Windows would treat as something else ({entry.FullName}); it was not unpacked.");
                var destination = Path.GetFullPath(Path.Combine(target, entry.FullName.Replace('/', '\\')));
                // Nothing leaves the folder it is unpacked into.
                if (!destination.StartsWith(fullTarget, StringComparison.OrdinalIgnoreCase) && destination.TrimEnd('\\') != fullTarget.TrimEnd('\\'))
                    throw new SetupException($"{name}'s archive tries to write outside its folder ({entry.FullName}); it was not unpacked.");
                if (entry.Name.Length == 0)
                {
                    Directory.CreateDirectory(destination);
                }
                else
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                    entry.ExtractToFile(destination, overwrite: true);
                }
                if (++done % 200 == 0) progress.Report((done, total));
            }
        }, ct);
        progress.Report((total, total));

        var unpacked = wrapped ? target : Path.Combine(temp, topFolder);
        File.WriteAllText(Path.Combine(unpacked, Marker), sha256 + "\n");
        SafeFolder.Delete(final);
        Retry.Io(() => Directory.Move(unpacked, final));
        SafeFolder.Delete(temp);
        Record(tools, name, topFolder, sha256);
        log($"{name}: unpacked into {final}");
        return final;
    }
}

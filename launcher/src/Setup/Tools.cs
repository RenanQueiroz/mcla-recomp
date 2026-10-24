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
                File.Move(partial, destination, overwrite: true);
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
        }
        if (topFolder.Contains("..") || Path.IsPathRooted(topFolder) || topFolder.Length == 0)
            throw new SetupException($"{name}'s archive has a name that can't be used.");
        var final = Path.Combine(tools, topFolder);
        var marker = Path.Combine(final, Marker);
        if (File.Exists(marker) && File.ReadAllText(marker).Trim() == sha256)
        {
            log($"{name}: already unpacked");
            progress.Report((total, total));
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
        Directory.Move(unpacked, final);
        SafeFolder.Delete(temp);
        log($"{name}: unpacked into {final}");
        return final;
    }
}

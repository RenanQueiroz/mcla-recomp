// The launcher updating itself, for a launcher that MCLA-Setup.exe installed (a folder with installed.txt beside the
// exe, normally %LOCALAPPDATA%\Programs\MCLA). Not used by the launcher built from source (Classic: it rebuilds
// itself with update-launcher.ps1) and not wired into the screens yet: the update check and its button are phase 5.
//
// The way an update goes:
//   1. the new MCLA-Setup.exe (a release asset, its SHA-256 in release.json) is fetched with DownloadAsync, which
//      refuses anything but the exact bytes (SHA-256 and size), and keeps it in DownloadFolder;
//   2. HandOver starts that file with --update --wait-pid <this process> --dir <this folder>, and the launcher
//      quits (the caller's job: nothing here exits the process);
//   3. MCLA-Setup.exe waits for this process to end, unpacks the new launcher beside the folder, swaps the two
//      (Mcla.Updating.FolderSwap), starts the new launcher and checks it came up; if it did not, the old folder is
//      put back and started again (installer\Program.cs);
//   4. on its next start the new launcher calls CleanUp, which removes what the swap could not delete while it ran
//      (the old folder) and the downloaded installers.
//
// Depends on nothing else in the launcher except installer\FolderSwap.cs, which the project links in.
using System.Diagnostics;
using System.Net;
using System.Security.Cryptography;
using Mcla.Updating;

namespace MclaLauncher;

/// A release asset to download: where it is and what it must be.
sealed record UpdateAsset(string Name, string Url, string Sha256, long Size);

sealed class UpdateException(string message, Exception? inner = null) : Exception(message, inner);

static class SelfUpdate
{
    /// The folder the running launcher's exe is in.
    public static string AppFolder => AppContext.BaseDirectory.TrimEnd('\\', '/');

    /// True when MCLA-Setup.exe put this launcher where it is (it leaves installed.txt in the folder).
    public static bool InstalledBySetup => File.Exists(Path.Combine(AppContext.BaseDirectory, "installed.txt"));

    /// The version of the installer that made this folder (installed.txt), or "" if unknown.
    public static string InstalledVersion => ReadInstalled("version");
    public static string InstalledTag => ReadInstalled("tag");

    static string ReadInstalled(string key)
    {
        try
        {
            foreach (var line in File.ReadAllLines(Path.Combine(AppContext.BaseDirectory, "installed.txt")))
                if (line.StartsWith(key + "=", StringComparison.Ordinal)) return line[(key.Length + 1)..].Trim();
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        return "";
    }

    /// Where downloaded installers wait: a sibling of the launcher's folder (same volume, never inside what is swapped).
    public static string DownloadFolder(string? appFolder = null) => (appFolder ?? AppFolder).TrimEnd('\\', '/') + ".download";

    /// Downloads the asset into `folder` and returns the path of the file, once it is exactly what `asset` says.
    /// https only (plain http is allowed for 127.0.0.1 when `allowLoopbackHttp`, for tests). Up to three tries for
    /// network trouble or a short file; bytes that are the right size but the wrong SHA-256 are not retried: whoever
    /// served them is not serving this release. Nothing is left behind on failure.
    public static async Task<string> DownloadAsync(UpdateAsset asset, string folder, Action<long, long>? progress, CancellationToken cancel,
        bool allowLoopbackHttp = false, HttpMessageHandler? handler = null, TimeSpan? stall = null, TimeSpan? retryDelay = null)
    {
        Validate(asset, allowLoopbackHttp);
        MakeDownloadFolder(folder);
        var final = Path.Combine(folder, asset.Name);
        if (File.Exists(final) && await IsExactlyAsync(final, asset, cancel)) return final; // an earlier try finished
        var part = final + ".part";
        Exception? last = null;
        for (var attempt = 1; attempt <= 3; attempt++)
        {
            try
            {
                await FetchAsync(asset, part, progress, cancel, handler, stall ?? TimeSpan.FromSeconds(60), allowLoopbackHttp);
                File.Move(part, final, overwrite: true);
                return final;
            }
            catch (UpdateException ex) when (ex.Message.StartsWith("The download is damaged", StringComparison.Ordinal))
            {
                TryDelete(part);
                throw; // right size, wrong hash: not a network problem
            }
            catch (Exception ex) when (ex is HttpRequestException or IOException or TimeoutException or UpdateException)
            {
                last = ex;
                TryDelete(part);
                if (attempt < 3) await Task.Delay((retryDelay ?? TimeSpan.FromSeconds(2)) * attempt, cancel);
            }
            catch { TryDelete(part); throw; }
        }
        throw new UpdateException($"Could not download {asset.Name}: {last?.Message}", last);
    }

    static async Task FetchAsync(UpdateAsset asset, string part, Action<long, long>? progress, CancellationToken cancel, HttpMessageHandler? handler, TimeSpan stall, bool allowLoopbackHttp)
    {
        // The shared client (Net.cs): both IP families raced, the system's proxy, redirects followed over https only; a failure
        // names the stage and the host (NetworkFailure, an HttpRequestException, so it is tried again like any network trouble).
        // The timeouts below are per step.
        using var own = handler != null ? NetClient.Create(handler) : null;
        var operation = new NetOperation($"download {asset.Name}");
        HttpResponseMessage response = await NetClient.GetAsync(asset.Url, operation,
            new NetRequest { Client = own, AnswerTimeout = TimeSpan.FromSeconds(30), AllowPlainHttp = allowLoopbackHttp }, cancel);
        using (response)
        {
            if (response.StatusCode != HttpStatusCode.OK)
                throw new UpdateException($"the server answered {(int)response.StatusCode} {response.ReasonPhrase}");
            if (response.Content.Headers.ContentLength is long advertised && advertised != asset.Size)
                throw new UpdateException($"The download is damaged or not the release's file (the server's file is {advertised} bytes, not {asset.Size}).");
            using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            long received = 0;
            await using (var network = await response.Content.ReadAsStreamAsync(cancel))
            await using (var file = new FileStream(part, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, FileOptions.Asynchronous))
            {
                var buffer = new byte[1 << 17];
                while (true)
                {
                    using var read = CancellationTokenSource.CreateLinkedTokenSource(cancel);
                    read.CancelAfter(stall);
                    int n;
                    try { n = await network.ReadAsync(buffer, read.Token); }
                    catch (OperationCanceledException) when (!cancel.IsCancellationRequested) { throw new TimeoutException($"nothing arrived for {stall.TotalSeconds:N0} seconds"); }
                    catch (Exception ex) when (ex is IOException or HttpRequestException) { throw NetClient.ReadFailure(ex, operation, stalled: false, stall); }
                    if (n == 0) break;
                    received += n;
                    if (received > asset.Size) throw new UpdateException($"The download is damaged or not the release's file (the server sent more than the {asset.Size} bytes of {asset.Name}).");
                    sha.AppendData(buffer, 0, n);
                    await file.WriteAsync(buffer.AsMemory(0, n), cancel);
                    progress?.Invoke(received, asset.Size);
                }
            }
            if (received != asset.Size) throw new UpdateException($"the file ended after {received} of {asset.Size} bytes");
            var actual = Convert.ToHexString(sha.GetHashAndReset());
            if (!actual.Equals(asset.Sha256, StringComparison.OrdinalIgnoreCase))
                throw new UpdateException($"The download is damaged or not the release's file (SHA-256 {actual.ToLowerInvariant()}, expected {asset.Sha256.ToLowerInvariant()}).");
        }
    }

    static async Task<bool> IsExactlyAsync(string path, UpdateAsset asset, CancellationToken cancel)
    {
        try
        {
            if (new FileInfo(path).Length != asset.Size) return false;
            await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.Asynchronous);
            return Convert.ToHexString(await SHA256.HashDataAsync(stream, cancel)).Equals(asset.Sha256, StringComparison.OrdinalIgnoreCase);
        }
        catch (IOException) { return false; }
    }

    static void Validate(UpdateAsset a, bool allowLoopbackHttp)
    {
        ValidateAsset(a);
        if (!Uri.TryCreate(a.Url, UriKind.Absolute, out var uri)) throw new UpdateException($"not an address: {a.Url}");
        var ok = uri.Scheme == Uri.UriSchemeHttps || (allowLoopbackHttp && uri.Scheme == Uri.UriSchemeHttp && uri.IsLoopback);
        if (!ok) throw new UpdateException("the installer must come from an https address");
    }

    /// The name, SHA-256 and size of an asset: all of them have to be there and plain.
    static void ValidateAsset(UpdateAsset a)
    {
        if (a.Name.Length == 0 || a.Name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || a.Name is "." or ".." || a.Name.EndsWith('.') || a.Name.EndsWith(' '))
            throw new UpdateException($"not a plain file name: {a.Name}");
        if (a.Sha256.Length != 64 || !a.Sha256.All(Uri.IsHexDigit)) throw new UpdateException("the release gives no SHA-256 for the installer");
        if (a.Size <= 0) throw new UpdateException("the release gives no size for the installer");
    }

    /// For a development release source (a folder, not a server): copies the file into `folder` and keeps it only if it is
    /// exactly what `asset` says (SHA-256 and size); returns the path. Nothing is left behind on failure.
    public static async Task<string> CopyVerifiedAsync(string sourceFile, UpdateAsset asset, string folder, CancellationToken cancel)
    {
        ValidateAsset(asset);
        if (!File.Exists(sourceFile)) throw new UpdateException($"{Path.GetFileName(sourceFile)} is missing from the release source.");
        MakeDownloadFolder(folder);
        var final = Path.Combine(folder, asset.Name);
        var part = final + ".part";
        try
        {
            File.Copy(sourceFile, part, overwrite: true);
            if (!await IsExactlyAsync(part, asset, cancel))
                throw new UpdateException("The download is damaged or not the release's file (the copy's SHA-256 or size is not what release.json says).");
            File.Move(part, final, overwrite: true);
            return final;
        }
        catch { TryDelete(part); throw; }
    }

    static void TryDelete(string path)
    {
        try { File.Delete(path); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }

    /// Starts the downloaded MCLA-Setup.exe as the updater for this launcher. The caller then closes the launcher
    /// (the installer waits for this process to end); until it has, nothing in the folder changes.
    /// `readyFile`: a path the new launcher is told (the MCLA_READY_FILE environment variable) and is to write once it
    /// has run well for a while (the wizard writes it 5 s after its start); the installer then takes the update as
    /// good only when the file is there (the way back otherwise). Without it the check is a window that is still
    /// there 3 s later.
    public static Process HandOver(string setupExe, string? appFolder = null, string? readyFile = null)
    {
        var folder = (appFolder ?? AppFolder).TrimEnd('\\', '/');
        var arguments = new List<string> { "--update", "--wait-pid", Environment.ProcessId.ToString(), "--dir", folder };
        if (readyFile != null) arguments.AddRange(["--ready-file", readyFile]);
        return StartSetup(setupExe, arguments);
    }

    /// For a launcher that Setup did not install (unpacked from the zip, in any folder): installs the launcher into
    /// Setup's own folder with this launcher's `install.json` as its pointer, so that the new launcher finds the same
    /// install. The old folder is left alone (the player may delete it). The caller then closes the launcher.
    public static Process MigrateTo(string setupExe, string installJson, string? readyFile = null)
    {
        var arguments = new List<string> { "--import-pointer", installJson };
        if (readyFile != null) arguments.AddRange(["--ready-file", readyFile]);
        return StartSetup(setupExe, arguments);
    }

    static Process StartSetup(string setupExe, IEnumerable<string> arguments)
    {
        var psi = new ProcessStartInfo(setupExe)
        {
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(setupExe)!,
        };
        foreach (var arg in arguments) psi.ArgumentList.Add(arg);
        return Process.Start(psi) ?? throw new UpdateException("Windows would not start the installer");
    }

    /// Why the installer would refuse to replace this launcher's folder, in plain words, or null when it would not. Looked at before the
    /// launcher quits to hand over, so that the reason is in the bar and the launcher keeps running: the folder has to be Setup's
    /// (installed.txt), must not hold the install its install.json names, and must hold nothing the setup did not unpack.
    public static string? WhyNotUpdatable(string? appFolder = null)
    {
        var folder = (appFolder ?? AppFolder).TrimEnd('\\', '/');
        try
        {
            if (!File.Exists(Path.Combine(folder, FolderSwap.OwnerMarker)))
                return $"This launcher was not put in {folder} by the setup, so the setup will not replace it.";
            if (FolderSwap.InstallRootInside(folder) is { } inside)
                return $"Your install ({inside}) is inside the launcher's folder, and an update would delete it along with the old launcher. Move the install, or the launcher's folder, first.";
            if (FolderSwap.ReadManifest(folder) is { } manifest)
            {
                var foreign = FolderSwap.ForeignFiles(folder, manifest, ["install.json", "settings.json"]);
                if (foreign.Count > 0)
                    return $"The launcher's folder holds things the setup did not put there ({string.Join(", ", foreign.Take(5))}{(foreign.Count > 5 ? ", ..." : "")}), and an update would delete them. Move them out first.";
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        return null;
    }

    static void MakeDownloadFolder(string folder)
    {
        Directory.CreateDirectory(folder);
        var marker = Path.Combine(folder, FolderSwap.DownloadMarker);
        if (!File.Exists(marker)) File.WriteAllText(marker, "Installers the launcher downloaded to update itself; the launcher removes this folder.\n");
    }

    /// True while a MCLA-Setup.exe is running for this user (it holds the Local\MCLA-Setup mutex for the whole run).
    public static bool SetupIsRunning()
    {
        try
        {
            if (!Mutex.TryOpenExisting(@"Local\MCLA-Setup", out var mutex)) return false;
            mutex.Dispose();
            return true;
        }
        catch (UnauthorizedAccessException) { return true; } // it exists, and is not ours to open
        catch (Exception) { return false; }
    }

    /// Removes the leftovers of an update beside the launcher's folder (the previous folder, a failed or half unpacked new one) and
    /// the installers it downloaded: only what carries Setup's own marker (FolderSwap.Leftovers) and the download folder this code
    /// made. Does nothing, and returns false, while a Setup is running: it may still need the previous folder to go back to, or be
    /// unpacking the new one. Quiet and best effort; what is still in use stays for the next start.
    public static bool CleanUp(string? appFolder = null)
    {
        try
        {
            if (SetupIsRunning()) return false;
            var folder = (appFolder ?? AppFolder).TrimEnd('\\', '/');
            FolderSwap.CleanLeftovers(folder);
            var downloads = DownloadFolder(folder);
            if (Directory.Exists(downloads) && File.Exists(Path.Combine(downloads, FolderSwap.DownloadMarker))) FolderSwap.DeleteFolder(downloads, TimeSpan.Zero);
        }
        catch (Exception) { /* a clean-up never stops the launcher */ }
        return true;
    }

    /// CleanUp, as soon as no Setup is running: looked at every few seconds, for a few minutes (the launcher that a Setup started
    /// is running while that Setup waits for it).
    public static void CleanUpWhenSetupIsDone(string? appFolder = null, int attempts = 100, int seconds = 3)
    {
        for (var i = 0; i < attempts; i++)
        {
            if (CleanUp(appFolder)) return;
            Thread.Sleep(TimeSpan.FromSeconds(seconds));
        }
    }
}

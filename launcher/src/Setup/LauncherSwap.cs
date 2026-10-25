// The launcher of a wizard install updating itself: the release's launcher package is downloaded and checked
// (SHA-256 and size from release.json), unpacked beside the launcher's own folder (<folder>.new), and a small
// PowerShell script, started as the launcher quits, swaps the folders (<folder> becomes <folder>.old, <folder>.new
// becomes <folder>), starts the new launcher and waits for it to say it started (MCLA_SWAP_MARKER, written by
// App.cs five seconds after the start). If it doesn't, the old one is put back and started again.
//
// This is the folder form. A single exe (a later package) is the same swap with one file: rename the exe to
// .old, the new one into its place, start it, wait for the marker, put the old one back if there is none. Only
// StageAsync (what is downloaded and unpacked) and the script's two renames are folder-specific.
using System.Diagnostics;
using System.IO.Compression;
using System.Text;

namespace MclaLauncher.Setup;

static class LauncherSwap
{
    public const string ExeName = "MclaLauncher.exe";
    public const string MarkerVariable = "MCLA_SWAP_MARKER";
    const long MaxUnpackedBytes = 1L << 30;
    const int MaxEntries = 20_000;

    /// The launcher's folder can be swapped: its parent can be written to (the new one is made beside it).
    public static bool CanSwap(string exeDirectory)
    {
        try
        {
            var parent = Path.GetDirectoryName(Path.GetFullPath(exeDirectory).TrimEnd('\\'));
            if (parent == null) return false;
            var probe = Path.Combine(parent, $".mcla-swap-test-{Environment.ProcessId}");
            Directory.CreateDirectory(probe);
            Directory.Delete(probe);
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException) { return false; }
    }

    /// Downloads and checks the launcher package, unpacks it into <exe folder>.new (with the pointer copied in) and
    /// returns that folder.
    public static async Task<string> StageAsync(InstallLayout layout, ReleaseSource source, ReleaseAsset asset, string exeDirectory,
                                                IProgress<long> progress, Action<string> log, CancellationToken ct)
    {
        var zipFile = Path.Combine(layout.Downloads, asset.Asset);
        await Verified.FetchAsync("the new launcher", zipFile, asset.Size, asset.Sha256,
            (dest, p, token) => source.FetchAssetAsync(asset.Asset, dest, p, token), progress, log, ct);
        var folder = Path.GetFullPath(exeDirectory).TrimEnd('\\');
        var staging = folder + ".new";
        SafeFolder.Delete(staging);
        Directory.CreateDirectory(staging);
        try
        {
            await Task.Run(() => Unpack(zipFile, staging, ct), ct);
            if (!File.Exists(Path.Combine(staging, ExeName)))
                throw new SetupException("The launcher package has no " + ExeName + "; it was not used.");
            var pointer = InstallRecord.PointerFile(folder);
            if (File.Exists(pointer)) File.Copy(pointer, InstallRecord.PointerFile(staging), overwrite: true);
        }
        catch
        {
            SafeFolder.Delete(staging);
            throw;
        }
        log($"the new launcher is unpacked in {staging}");
        return staging;
    }

    static void Unpack(string zipFile, string destination, CancellationToken ct)
    {
        using var zip = ZipFile.OpenRead(zipFile);
        if (zip.Entries.Count > MaxEntries || zip.Entries.Sum(e => e.Length) > MaxUnpackedBytes)
            throw new SetupException("The launcher package is far bigger than a launcher should be; it was not unpacked.");
        var root = Path.GetFullPath(destination).TrimEnd('\\') + "\\";
        foreach (var entry in zip.Entries)
        {
            ct.ThrowIfCancellationRequested();
            if (ToolInstaller.IsOddName(entry.FullName))
                throw new SetupException($"The launcher package has a file name Windows would treat as something else ({entry.FullName}); it was not unpacked.");
            var target = Path.GetFullPath(Path.Combine(destination, entry.FullName.Replace('/', '\\')));
            if (!target.StartsWith(root, StringComparison.OrdinalIgnoreCase) && target.TrimEnd('\\') != root.TrimEnd('\\'))
                throw new SetupException($"The launcher package tries to write outside its folder ({entry.FullName}); it was not unpacked.");
            if (entry.Name.Length == 0) { Directory.CreateDirectory(target); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            entry.ExtractToFile(target, overwrite: true);
        }
    }

    /// Starts the swap script for the launcher process `processId` (which quits right after this) and returns the log
    /// the script writes. The caller exits.
    public static string StartSwap(InstallLayout layout, string stagingFolder, string exeDirectory, int processId, int secondsToWait = 25)
    {
        Directory.CreateDirectory(layout.Downloads);
        var script = Path.Combine(layout.Downloads, "swap-launcher.ps1");
        var log = Path.Combine(layout.Downloads, "swap-launcher.log");
        var marker = Path.Combine(layout.Downloads, "swap-launcher.ok");
        File.WriteAllText(script, Script, new UTF8Encoding(true));
        File.Delete(log);
        var psi = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, CreateNoWindow = true };
        foreach (var arg in new[]
        {
            "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
            "-WaitFor", processId.ToString(), "-Exe", Path.GetFullPath(exeDirectory).TrimEnd('\\'), "-New", stagingFolder,
            "-Name", ExeName, "-Marker", marker, "-Seconds", secondsToWait.ToString(), "-Log", log,
        }) psi.ArgumentList.Add(arg);
        Process.Start(psi);
        return log;
    }

    /// Written to downloads\swap-launcher.ps1. ASCII only (Windows PowerShell reads a file without a mark as ANSI).
    internal const string Script = """
        param([int]$WaitFor, [string]$Exe, [string]$New, [string]$Name, [string]$Marker, [int]$Seconds = 25, [string]$Log)
        $ErrorActionPreference = 'Stop'
        function Say([string]$text) { Add-Content -LiteralPath $Log -Value ('[{0:HH:mm:ss}] {1}' -f (Get-Date), $text) }
        # A scanner may hold a file of a folder just unpacked or a program just closed: try again for ten seconds.
        function Retry([scriptblock]$action) {
            for ($i = 0; $i -lt 40; $i++) { try { & $action; return } catch { Start-Sleep -Milliseconds 250 } }
            & $action
        }
        $old = $Exe + '.old'
        $exeName = [IO.Path]::GetFileName($Exe)
        try {
            Say "waiting for the launcher ($WaitFor) to close"
            if ($WaitFor) { Wait-Process -Id $WaitFor -ErrorAction SilentlyContinue -Timeout 60 }
            if (Test-Path -LiteralPath $old) { Retry { Remove-Item -LiteralPath $old -Recurse -Force } }
            Retry { Rename-Item -LiteralPath $Exe -NewName ([IO.Path]::GetFileName($old)) }
            Say "the old launcher is now $old"
            try { Retry { Rename-Item -LiteralPath $New -NewName $exeName } }
            catch { Rename-Item -LiteralPath $old -NewName $exeName; throw }
            Remove-Item -LiteralPath $Marker -ErrorAction SilentlyContinue
            $env:MCLA_SWAP_MARKER = $Marker
            $p = Start-Process -FilePath (Join-Path $Exe $Name) -WorkingDirectory $Exe -PassThru
            $env:MCLA_SWAP_MARKER = $null
            $deadline = (Get-Date).AddSeconds($Seconds)
            while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $Marker) -and -not $p.HasExited) { Start-Sleep -Milliseconds 300 }
            if (Test-Path -LiteralPath $Marker) {
                Say "the new launcher started"
                Retry { Remove-Item -LiteralPath $old -Recurse -Force }
                Say "done"
                exit 0
            }
            Say "the new launcher did not start (it exited: $($p.HasExited)); putting the old one back"
            if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
            Wait-Process -Id $p.Id -ErrorAction SilentlyContinue -Timeout 10
            $failed = $Exe + '.failed'
            if (Test-Path -LiteralPath $failed) { Retry { Remove-Item -LiteralPath $failed -Recurse -Force } }
            Retry { Rename-Item -LiteralPath $Exe -NewName ([IO.Path]::GetFileName($failed)) }
            Retry { Rename-Item -LiteralPath $old -NewName $exeName }
            Say "the old launcher is back; starting it"
            Start-Process -FilePath (Join-Path $Exe $Name) -WorkingDirectory $Exe
            exit 1
        } catch {
            Say "FAILED: $($_.Exception.Message)"
            try {
                if (-not (Test-Path -LiteralPath $Exe) -and (Test-Path -LiteralPath $old)) { Rename-Item -LiteralPath $old -NewName $exeName }
                if (Test-Path -LiteralPath (Join-Path $Exe $Name)) { Start-Process -FilePath (Join-Path $Exe $Name) -WorkingDirectory $Exe }
            } catch { Say "could not put the old launcher back: $($_.Exception.Message)" }
            exit 1
        }
        """;
}

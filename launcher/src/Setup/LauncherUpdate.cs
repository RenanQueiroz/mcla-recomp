// The launcher of a wizard install and its own updates. The launcher never swaps its own folder: it can't know what else is
// in it (the zip is flat and "unpacked anywhere"; a player may keep the install, or other things, beside or inside it).
// MCLA-Setup.exe does it (launcher\installer, SelfUpdate.cs): the launcher downloads the release's MCLA-Setup.exe, checks its
// SHA-256 and size against release.json, hands over to it and quits; the installer unpacks the new launcher beside the folder,
// swaps the two, starts the new launcher and goes back to the old one if the new one doesn't say it started.
//
// The bar in LauncherApp is shown when the release's launcher (release.json's launcher.sourceCommit) is not the one running
// (the source-commit.txt beside the exe). What its button does depends on how this launcher got where it is (Way):
//   InPlace      Setup installed it (installed.txt beside the exe): MCLA-Setup.exe --update replaces this folder.
//   Move         it was unpacked from a zip, or made by an older wizard: it can't be replaced in place, so MCLA-Setup.exe
//                installs the new launcher in Setup's own folder with this one's install.json as its pointer
//                (--import-pointer); the old folder stays for the player to delete.
//   ReleasePage  the release has no installer (or InPlace is off): the button opens the release's page.
// The new launcher is told a file's name in ReadyFileVariable and writes it five seconds after it is up (App.cs); the installer
// takes that as "it started" (and puts the old launcher back if the file doesn't come).
namespace MclaLauncher.Setup;

static class LauncherUpdate
{
    /// The variable that names the file a launcher started by an updater writes when it has run for five seconds.
    public const string ReadyFileVariable = "MCLA_READY_FILE";

    /// Whether the launcher can update itself through the installer (the hand-over is wired in).
    public static bool InPlace => true;

    public enum Way { ReleasePage, InPlace, Move }

    /// Where MCLA-Setup.exe puts the launcher it installs (its default, or what a test sets in MCLA_SETUP_DIR).
    static string SetupFolder => Environment.GetEnvironmentVariable("MCLA_SETUP_DIR") is { Length: > 0 } dir
        ? dir
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "MCLA");

    public static string ReleasePage(ReleaseInfo release) => $"{Paths.RepoUrl}/releases/tag/{Uri.EscapeDataString(release.Tag)}";

    /// What the bar's button does for this launcher and this release.
    public static Way WayFor(ReleaseInfo release) =>
        !InPlace || release.Installer == null ? Way.ReleasePage
        : SelfUpdate.InstalledBySetup ? Way.InPlace
        : Way.Move;

    /// Where the new launcher says it started: a file of its own in the temp folder, outside everything the installer swaps
    /// or the new launcher cleans up.
    public static string NewReadyFile() => Path.Combine(Path.GetTempPath(), $"mcla-launcher-ready-{Environment.ProcessId}.txt");

    /// Downloads the release's MCLA-Setup.exe (copied, for a development release source that is a folder) and checks it
    /// against release.json; returns the path of the file. Nothing is run.
    public static async Task<string> FetchInstallerAsync(ReleaseInfo release, ReleaseSource source, Action<long, long>? progress, CancellationToken ct)
    {
        var installer = release.Installer ?? throw new SetupException("This release has no installer for the launcher.");
        var asset = new UpdateAsset(installer.Asset, source.LocalFolder == null ? source.AssetUrl(installer.Asset) : "", installer.Sha256, installer.Size);
        // Beside the folder the new launcher will be in, where its clean-up (App.cs) looks for it afterwards.
        var folder = SelfUpdate.DownloadFolder(SelfUpdate.InstalledBySetup ? null : SetupFolder);
        try
        {
            return source.LocalFolder is { } local
                ? await SelfUpdate.CopyVerifiedAsync(Path.Combine(local, installer.Asset), asset, folder, ct)
                : await SelfUpdate.DownloadAsync(asset, folder, progress, ct);
        }
        catch (UpdateException ex)
        {
            throw new SetupException($"The new launcher could not be fetched. {ex.Message}", ex.Message, ex);
        }
    }

    /// Starts the installer for `way` (InPlace or Move) with the ready file; the caller then closes this launcher.
    public static void Start(Way way, string setupExe, string readyFile)
    {
        try
        {
            if (way == Way.InPlace) SelfUpdate.HandOver(setupExe, readyFile: readyFile);
            else SelfUpdate.MigrateTo(setupExe, InstallRecord.PointerFile(AppContext.BaseDirectory), readyFile);
        }
        catch (Exception ex) when (ex is UpdateException or System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            throw new SetupException("Windows would not start the installer of the new launcher.", ex.Message, ex);
        }
    }
}

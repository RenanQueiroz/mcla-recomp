// The launcher of a wizard install and its own updates. The launcher never swaps its own folder: it can't know what else is
// in it (the zip is flat and "unpacked anywhere"; a player may keep the install, or other things, beside or inside it).
//
// What exists: the bar in LauncherApp, shown when the release's launcher (release.json's launcher.sourceCommit) is not the
// one running (the source-commit.txt beside the exe); its button opens the release's page.
//
// The seam for the installer (phase 6: MCLA-Setup.exe replaces a launcher it installed, with a start check and a way back):
// when InPlace is true, the bar's button downloads MCLA-Setup.exe from the release (SelfUpdate.DownloadAsync, SHA-256 and size
// from release.json's installer), hands over to it (SelfUpdate.HandOver, passing a ready file named by ReadyFileVariable's
// value) and exits; the launcher it starts writes that file five seconds after it is up (App.cs), or the installer puts the
// old launcher back. Nothing else in the launcher knows how the swap is done.
namespace MclaLauncher.Setup;

static class LauncherUpdate
{
    /// The variable that names the file a launcher started by an updater writes when it has run for five seconds.
    public const string ReadyFileVariable = "MCLA_READY_FILE";

    /// Whether the launcher can update itself in place (the installer's hand-over is wired in).
    public static bool InPlace => false;

    public static string ReleasePage(ReleaseInfo release) => $"{Paths.RepoUrl}/releases/tag/{Uri.EscapeDataString(release.Tag)}";
}

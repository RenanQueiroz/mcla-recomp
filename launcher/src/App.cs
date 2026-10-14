// Midnight Club: Los Angeles launcher. Checks mcla-recomp for updates, offers
// to update and rebuild the game, then starts it.
using MclaLauncher;
using Microsoft.UI.Reactor;
using Microsoft.UI.Reactor.Core;

ReactorApp.Run<LauncherApp>(new WindowSpec
{
    Title = "Midnight Club: Los Angeles",
    Width = 920,
    Height = 720,
    MinWidth = 720,
    MinHeight = 600,
    Backdrop = BackdropChoice.Of(BackdropKind.Mica),
    Icon = File.Exists(Paths.Icon) ? WindowIcon.FromPath(Paths.Icon) : null,
    StartPosition = WindowStartPosition.CenterOnPrimary,
});

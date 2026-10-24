// Midnight Club: Los Angeles launcher. Checks mcla-recomp for updates, offers
// to update and rebuild the game, then starts it. On a PC with no install it
// starts the setup wizard instead (Paths.Mode; see Backend.cs).
using MclaLauncher;
using Microsoft.UI.Reactor;
using Microsoft.UI.Reactor.Core;

var wizard = Paths.Mode == LaunchMode.Wizard;
var spec = new WindowSpec
{
    Title = "Midnight Club: Los Angeles",
    Width = wizard ? 860 : 920,
    Height = wizard ? 820 : 800,
    MinWidth = 720,
    MinHeight = 600,
    Backdrop = BackdropChoice.Of(BackdropKind.Mica),
    Icon = File.Exists(Paths.Icon) ? WindowIcon.FromPath(Paths.Icon) : null,
    StartPosition = WindowStartPosition.CenterOnPrimary,
};
if (wizard) ReactorApp.Run<WizardApp>(spec);
else ReactorApp.Run<LauncherApp>(spec);

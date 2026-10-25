// Midnight Club: Los Angeles launcher. Checks mcla-recomp for updates, offers
// to update and rebuild the game, then starts it. On a PC with no install it
// starts the setup wizard instead (Paths.Mode; see Backend.cs).
using MclaLauncher;
using Microsoft.UI.Reactor;
using Microsoft.UI.Reactor.Core;

// A launcher that replaced the one before it says it started: five seconds alive. Whatever replaced it (the installer, once
// it does the update: Setup\LauncherUpdate.cs) puts the old launcher back when the file doesn't appear. Not passed on to the
// game or anything else this launcher starts.
if (Environment.GetEnvironmentVariable(MclaLauncher.Setup.LauncherUpdate.ReadyFileVariable) is { Length: > 0 } swapMarker)
{
    Environment.SetEnvironmentVariable(MclaLauncher.Setup.LauncherUpdate.ReadyFileVariable, null);
    _ = Task.Delay(5000).ContinueWith(_ =>
    {
        try { File.WriteAllText(swapMarker, "started"); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    });
}

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

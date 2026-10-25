// Midnight Club: Los Angeles launcher. Checks mcla-recomp for updates, offers
// to update and rebuild the game, then starts it. On a PC with no install it
// starts the setup wizard instead (Paths.Mode; see Backend.cs).
using MclaLauncher;
using Microsoft.UI.Reactor;
using Microsoft.UI.Reactor.Core;

// A launcher that replaced the one before it says it started: from its own window's thread, five seconds after the window opened
// (a launcher whose window never comes, or whose UI thread is stuck, never writes the file). Whatever replaced it (MCLA-Setup.exe,
// Setup\LauncherUpdate.cs) puts the old launcher back when the file doesn't appear. Not passed on to the game or anything else this
// launcher starts.
if (Environment.GetEnvironmentVariable(MclaLauncher.Setup.LauncherUpdate.ReadyFileVariable) is { Length: > 0 } swapMarker)
{
    Environment.SetEnvironmentVariable(MclaLauncher.Setup.LauncherUpdate.ReadyFileVariable, null);
    object? readyTimer = null;
    ReactorApp.WindowOpened += (_, _) =>
    {
        if (readyTimer != null) return;
        var timer = ReactorApp.UIDispatcher.CreateTimer();
        timer.Interval = TimeSpan.FromSeconds(5);
        timer.IsRepeating = false;
        timer.Tick += (_, _) =>
        {
            try { File.WriteAllText(swapMarker, "started"); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        };
        timer.Start();
        readyTimer = timer;
    };
}

// What an update of this launcher (MCLA-Setup.exe, SelfUpdate.cs) leaves beside its folder: the previous folder, a failed one, the
// downloaded installer. Not while a Setup is running (the one that started this launcher may still need the previous folder to
// go back to): it waits until none is.
if (SelfUpdate.InstalledBySetup)
    _ = Task.Run(() => SelfUpdate.CleanUpWhenSetupIsDone());

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

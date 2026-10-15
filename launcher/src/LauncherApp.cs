using System.Collections.Immutable;
using System.Diagnostics;
using Microsoft.UI.Reactor;
using Microsoft.UI.Reactor.Core;
using Microsoft.UI.Reactor.Hooks;
using Microsoft.UI.Reactor.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using static Microsoft.UI.Reactor.Factories;
using MenuItemBase = Microsoft.UI.Reactor.Core.MenuFlyoutItemBase;

namespace MclaLauncher;

sealed class LauncherApp : Component
{
    const int MaxLogLines = 2000;
    const string Mono = "Cascadia Mono, Consolas";

    public override Element Render()
    {
        var initialSettings = UseMemo(LauncherSettings.Load, Array.Empty<object>());
        var (settings, updateSettings) = UseReducer(initialSettings);
        var (status, setStatus) = UseState<RepoStatus?>(null);
        var (checking, setChecking) = UseState(true);
        var (error, setError) = UseState<string?>(null);
        var (notice, setNotice) = UseState<string?>(null);
        var (build, setBuild) = UseState<BuildRun?>(null);
        var (buildResult, setBuildResult) = UseState<int?>(null);
        var (log, updateLog) = UseReducer(ImmutableList<string>.Empty, threadSafe: true);
        var (step, setStep) = UseState<string?>(null);
        var (activity, setActivity) = UseState<Activity?>(null);
        var (gameRunning, setGameRunning) = UseState(false);
        var (countdown, setCountdown) = UseState<int?>(null);
        var (confirmExit, setConfirmExit) = UseState(false);
        var buildRef = UseRef<BuildRun?>(null);
        var exitingRef = UseRef(false);
        var autoPlayDoneRef = UseRef(false);
        var followLogRef = UseRef(true);
        var logScroller = this.UseElementRef<ScrollViewer>();
        var logText = UseMemo(() => string.Join("\n", log), log);

        var busy = build != null && buildResult == null;
        var failed = buildResult is int exit && exit != 0 && build?.StopRequested != true;
        var gameBuilt = status?.GameBuilt ?? File.Exists(Paths.GameExe);
        var readyToPlay = status is { GameBuilt: true, BuildCurrent: true, UpdateAvailable: false }
                          && !checking && !busy && build == null && error == null;

        async Task CheckAsync(bool fetch)
        {
            setChecking(true);
            try
            {
                setStatus(await Task.Run(() => Repo.GetStatusAsync(fetch)));
                setError(null);
            }
            catch (Exception ex) when (ex is IOException or TimeoutException or InvalidOperationException or System.ComponentModel.Win32Exception)
            {
                setError(ex.Message);
            }
            finally
            {
                setChecking(false);
            }
        }

        void StartBuild(BuildKind kind)
        {
            if (Process.GetProcessesByName("mcla").Length > 0)
            {
                setNotice("Close the game first: the build can't replace mcla.exe while it's running.");
                return;
            }
            setNotice(null);
            setCountdown(null);
            setStep(null);
            setActivity(null);
            setBuildResult(null);
            updateLog(_ => ImmutableList<string>.Empty);
            followLogRef.Current = true;
            BuildRun run;
            try
            {
                run = new BuildRun(kind, line =>
                {
                    updateLog(lines => (lines.Count >= MaxLogLines ? lines.RemoveAt(0) : lines).Add(line));
                    if (line.StartsWith("==> ")) setStep(line[4..]);
                });
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            {
                setNotice($"Couldn't start the build: {ex.Message}");
                return;
            }
            buildRef.Current = run;
            setBuild(run);
            _ = run.Completion.ContinueWith(task =>
            {
                setBuildResult(task.Result);
                _ = CheckAsync(fetch: false);
            }, TaskScheduler.Default);
        }

        void Play()
        {
            setCountdown(null);
            try
            {
                var game = Process.Start(new ProcessStartInfo(Paths.GameExe, GameOptions.Arguments(settings))
                {
                    WorkingDirectory = Paths.BuildDir,
                    UseShellExecute = true,
                });
                if (settings.CloseOnLaunch)
                {
                    exitingRef.Current = true;
                    ReactorApp.Exit();
                    return;
                }
                setGameRunning(true);
                game?.WaitForExitAsync().ContinueWith(_ => setGameRunning(false), TaskScheduler.Default);
            }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
            {
                setNotice($"Couldn't start the game: {ex.Message}");
            }
        }

        void Open(string target)
        {
            try
            {
                if (!target.StartsWith("http")) Directory.CreateDirectory(Path.HasExtension(target) ? Path.GetDirectoryName(target)! : target);
                Process.Start(new ProcessStartInfo(target) { UseShellExecute = true });
            }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or IOException)
            {
                setNotice($"Couldn't open {target}: {ex.Message}");
            }
        }

        // For what the launcher can't do: answering make_pc.ps1's questions
        // (such as installing a missing tool) needs a real console.
        void BuildInTerminal()
        {
            var psi = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, WorkingDirectory = Paths.LauncherDir };
            foreach (var arg in new[] { "-NoExit", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Paths.BuildScript })
                psi.ArgumentList.Add(arg);
            Process.Start(psi);
        }

        // An update changed the launcher itself: it can't replace its own files
        // while running, so update-launcher.ps1 waits for it to quit, rebuilds
        // it and starts the new one.
        void RestartToUpdateLauncher()
        {
            try
            {
                var psi = new ProcessStartInfo("powershell.exe") { UseShellExecute = false, WorkingDirectory = Paths.LauncherDir };
                foreach (var arg in new[] { "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", Paths.UpdateLauncherScript,
                                            "-WaitFor", Environment.ProcessId.ToString() })
                    psi.ArgumentList.Add(arg);
                Process.Start(psi);
                exitingRef.Current = true;
                ReactorApp.Exit();
            }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
            {
                setNotice($"Couldn't update the launcher: {ex.Message}");
            }
        }

        // Once, when the window opens: a title bar that follows dark mode
        // (set on the AppWindow, as Reactor's published previews have no
        // WindowSpec option for it yet), and the update check.
        UseEffect(() =>
        {
            if (ReactorApp.PrimaryWindow?.AppWindow is { } appWindow)
                appWindow.TitleBar.PreferredTheme = Microsoft.UI.Windowing.TitleBarTheme.UseDefaultAppMode;
            _ = CheckAsync(settings.CheckForUpdates);
        }, Array.Empty<object>());

        UseEffect(() =>
        {
            try { settings.Save(); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { Debug.WriteLine($"settings: {ex.Message}"); }
        }, settings);

        // Closing the window mid-build asks first; quitting stops the build.
        UseEffect(() =>
        {
            var window = ReactorApp.PrimaryWindow;
            if (window == null) return () => { };
            void OnClosing(object? sender, WindowClosingEventArgs e)
            {
                if (exitingRef.Current || buildRef.Current is not { Completion.IsCompleted: false }) return;
                e.Cancel = true;
                setConfirmExit(true);
            }
            window.Closing += OnClosing;
            return () => window.Closing -= OnClosing;
        }, Array.Empty<object>());

        // While building, follow the newest log under mcla-recomp\out\ once a second.
        UseEffect(() =>
        {
            if (build == null || buildResult != null) return () => { };
            var cts = new CancellationTokenSource();
            var since = build.StartedUtc;
            _ = Task.Run(async () =>
            {
                using var timer = new PeriodicTimer(TimeSpan.FromSeconds(1));
                try
                {
                    while (await timer.WaitForNextTickAsync(cts.Token))
                        setActivity(Activity.Read(since));
                }
                catch (Exception ex) when (ex is OperationCanceledException or InvalidOperationException) { }
            });
            return () => cts.Cancel();
        }, build, buildResult);

        // Mirror the build's progress on the taskbar button.
        UseEffect(() =>
        {
            var progress = ReactorApp.PrimaryWindow?.Progress;
            if (progress == null) return;
            if (busy && activity?.Percent is double percent)
            {
                progress.State = TaskbarProgressState.Normal;
                progress.Value = Math.Clamp(percent / 100, 0, 1);
            }
            else if (busy)
            {
                progress.State = TaskbarProgressState.Indeterminate;
            }
            else if (failed)
            {
                progress.State = TaskbarProgressState.Error;
                progress.Value = 1;
            }
            else
            {
                progress.Clear();
            }
        }, busy, activity, failed);

        // Keep the build output scrolled to the end unless the user scrolled up.
        UseEffect(() =>
        {
            var scroller = logScroller.Current;
            if (scroller == null || !followLogRef.Current) return;
            scroller.DispatcherQueue.TryEnqueue(() =>
            {
                scroller.UpdateLayout();
                scroller.ChangeView(null, scroller.ScrollableHeight, null, disableAnimation: true);
            });
        }, log);

        // "Start the game right away": a short countdown that can be cancelled.
        UseEffect(() =>
        {
            if (autoPlayDoneRef.Current || !readyToPlay || !settings.AutoPlay) return;
            autoPlayDoneRef.Current = true;
            setCountdown(3);
        }, readyToPlay);

        UseEffect(() =>
        {
            if (countdown is not int seconds) return () => { };
            if (seconds <= 0)
            {
                Play();
                return () => { };
            }
            var cts = new CancellationTokenSource();
            _ = Task.Delay(1000, cts.Token).ContinueWith(t =>
            {
                if (!t.IsCanceled) setCountdown(seconds - 1);
            }, TaskScheduler.Default);
            return () => cts.Cancel();
        }, countdown);

        // ------------------------------------------------------------------
        // Layout

        Element Header()
        {
            var head = status?.Recent.FirstOrDefault();
            var subtitle = head is null
                ? "Static recompilation for PC"
                : $"Static recompilation for PC  ·  version {head.Hash} from {head.Date}";
            // Routine states read as a line under the title; the bars below are
            // for things that need a decision.
            var state = checking ? "checking for updates…"
                      : error != null || busy || status == null || failed || build?.StopRequested == true || buildResult == 0 ? null
                      : status.UpdateAvailable || !status.GameBuilt || !status.BuildCurrent ? null
                      : status.FetchError != null ? "couldn't check for updates"
                      : "up to date";
            if (state != null) subtitle += $"  ·  {state}";
            // The fork takes upstream's changes only when asked to (FORK.md),
            // so this is information, not an update.
            var upstream = status?.UpstreamAhead switch
            {
                null or 0 => "Upstream mcla-recomp on GitHub",
                1 => "Upstream mcla-recomp on GitHub (1 commit not in your fork)",
                var n => $"Upstream mcla-recomp on GitHub ({n} commits not in your fork)",
            };
            MenuItemBase[] items = busy
                ? [MenuItem("Open the game folder", () => Open(Paths.BuildDir)),
                   MenuItem("Open the launcher logs", () => Open(Paths.LogsDir)),
                   MenuItem("Recent changes on GitHub", () => Open(Paths.RepoUrl + "/commits/main")),
                   MenuItem(upstream, () => Open(Paths.UpstreamUrl))]
                : [MenuItem("Check for updates", () => _ = CheckAsync(fetch: true)),
                   MenuItem("Rebuild the game", () => StartBuild(BuildKind.Build)),
                   MenuItem("Re-patch and rebuild the SDK", () => StartBuild(BuildKind.ResetSdk)),
                   MenuItem("Run the build in a terminal", BuildInTerminal),
                   MenuSeparator(),
                   MenuItem("Open the game folder", () => Open(Paths.BuildDir)),
                   MenuItem("Open the launcher logs", () => Open(Paths.LogsDir)),
                   MenuItem("Recent changes on GitHub", () => Open(Paths.RepoUrl + "/commits/main")),
                   MenuItem(upstream, () => Open(Paths.UpstreamUrl))];
            return Grid(
                columns: [GridSize.Star(), GridSize.Auto],
                rows: [GridSize.Auto],
                VStack(2,
                    Title("Midnight Club: Los Angeles"),
                    TextBlock(subtitle).Foreground(Theme.SecondaryText)
                ).Grid(column: 0),
                DropDownButton("More", MenuItems(items)).VAlign(VerticalAlignment.Center).Grid(column: 1));
        }

        static Element Info(string title, string? message, InfoBarSeverity severity) =>
            (InfoBar(title, message) with { IsOpen = true, IsClosable = false }).Severity(severity);

        Element StatusBar()
        {
            if (error != null)
                return Info("Can't read the mcla-recomp folder", error, InfoBarSeverity.Error);
            if (busy)
                return Info(build!.Kind == BuildKind.Update ? "Updating and rebuilding" : "Building",
                            step is null ? "Starting…" : $"Step {step}", InfoBarSeverity.Informational);
            if (build?.StopRequested == true)
                return Info("Build stopped", "Finished steps are kept, so the next build continues where this one stopped.", InfoBarSeverity.Warning);
            if (failed)
                return Info("The build failed", "The output below shows where. Play still starts the last version that built.", InfoBarSeverity.Error);
            if (checking || status == null)
                return Empty();
            if (status.UpdateAvailable)
                return Info(status.Incoming.Count == 1 ? "An update is available" : $"{status.Incoming.Count} updates are available",
                            "Updating downloads the changes and rebuilds the game, which takes a few minutes."
                            + (status.PatchesChanged ? " This update changes the SDK patches, so the SDK is rebuilt too." : ""),
                            InfoBarSeverity.Informational);
            if (!status.GameBuilt)
                return Info("The game hasn't been built yet",
                            "Building extracts the game from your disc image and recompiles it. The first build takes a while.",
                            InfoBarSeverity.Warning);
            if (!status.BuildCurrent)
                return Info("The last build didn't finish", "Rebuild to finish it. Play starts the last version that built.", InfoBarSeverity.Warning);
            if (buildResult == 0)
                return Info("Build finished", $"You're on version {status.Head[..7]}.", InfoBarSeverity.Success);
            return Empty();
        }

        // The update's commits, folded away under its bar.
        Element? WhatsNew() => status is { UpdateAvailable: true } && !busy
            ? Expander(status.Incoming.Count == 1 ? "What's new (1 change)" : $"What's new ({status.Incoming.Count} changes)",
                       ScrollViewer(VStack(10, status.Incoming.Select(c => CommitRow(c).WithKey(c.Hash)).ToArray())).MaxHeight(220),
                       isExpanded: false)
                .HAlign(HorizontalAlignment.Stretch).HorizontalContentAlignment(HorizontalAlignment.Stretch)
            : null;

        Element? LauncherUpdateBar() => status is { LauncherOutdated: true } && !busy
            ? (InfoBar("The launcher itself was updated", "Restarting rebuilds it (about a minute) and opens the new one.") with
              {
                  IsOpen = true,
                  IsClosable = false,
                  ActionButtonContent = "Restart to update the launcher",
                  OnActionButtonClick = RestartToUpdateLauncher,
              }).Severity(InfoBarSeverity.Informational)
            : null;

        Element? CountdownBar() => countdown is int seconds
            ? (InfoBar($"Starting the game in {seconds}…", "You can turn this off under Advanced.") with
              {
                  IsOpen = true,
                  IsClosable = false,
                  ActionButtonContent = "Cancel",
                  OnActionButtonClick = () => setCountdown(null),
              }).Severity(InfoBarSeverity.Informational)
            : null;

        Element? NoticeBar() => notice is null
            ? null
            : (InfoBar("Something went wrong", notice) with { IsOpen = true, IsClosable = true, OnClosed = () => setNotice(null) })
                .Severity(InfoBarSeverity.Error);

        static Element CommitRow(Commit commit) =>
            Grid(
                columns: [GridSize.Px(80), GridSize.Px(96), GridSize.Star()],
                rows: [GridSize.Auto],
                TextBlock(commit.Hash).FontFamily(Mono).Foreground(Theme.SecondaryText).Grid(column: 0),
                TextBlock(commit.Date).Foreground(Theme.SecondaryText).Grid(column: 1),
                TextBlock(commit.Subject).TextWrapping().Grid(column: 2));

        static Element Pane(Element heading, Element body) =>
            Card(Grid(
                columns: [GridSize.Star()],
                rows: [GridSize.Auto, GridSize.Star()],
                heading.Margin(0, 0, 0, 12).Grid(row: 0),
                body.Grid(row: 1))
            ).Margin(0, 16, 0, 0);

        Element LogPane() =>
            Pane(Grid(
                    columns: [GridSize.Star(), GridSize.Auto],
                    rows: [GridSize.Auto],
                    BodyStrong("Build output").VAlign(VerticalAlignment.Center).Grid(column: 0),
                    HyperlinkButton("Open the full log", onClick: () => Open(build!.LogFile)).Grid(column: 1)),
                 ScrollViewer(TextBlock(logText).FontFamily(Mono).FontSize(12).IsTextSelectionEnabled().TextWrapping())
                    .ViewChanged(e =>
                    {
                        if (e.IsIntermediate || logScroller.Current is not { } scroller) return;
                        followLogRef.Current = scroller.ScrollableHeight - scroller.VerticalOffset < 32;
                    })
                    .Ref(logScroller));

        Element MainPane()
        {
            if (build != null && (busy || failed)) return LogPane();
            return Settings();
        }

        Element ActivityRow()
        {
            if (!busy) return Empty();
            var text = activity is null ? "Working…"
                     : activity.Detail.Length == 0 ? activity.Step
                     : $"{activity.Step}  ·  {activity.Detail}";
            return VStack(6,
                activity?.Percent is double percent ? Progress(percent) : ProgressIndeterminate(),
                TextBlock(text).Foreground(Theme.SecondaryText).TextTrimming(TextTrimming.CharacterEllipsis).MaxLines(1)
            ).Margin(0, 12, 0, 0);
        }

        Element Combo(string header, Choice[] choices, int index, Func<LauncherSettings, int, LauncherSettings> apply) =>
            ComboBox(choices.Select(c => c.Label).ToArray(), Math.Clamp(index, 0, choices.Length - 1),
                     i => { if (i >= 0) updateSettings(s => apply(s, i)); })
                .Header(header)
                .HAlign(HorizontalAlignment.Stretch);

        // Two to a row, in reading order.
        static Element Pairs(params Element[] items) =>
            Grid(
                columns: [GridSize.Star(), GridSize.Star()],
                rows: Enumerable.Repeat(GridSize.Auto, (items.Length + 1) / 2).ToArray(),
                items.Select((item, i) => item.Margin(i % 2 == 0 ? 0 : 8, 0, i % 2 == 0 ? 8 : 0, 12).Grid(row: i / 2, column: i % 2)
                                              .WithKey(i.ToString())).ToArray());

        Element Settings() =>
            ScrollViewer(VStack(12,
                Subtitle("Graphics"),
                Pairs(
                    Combo("Resolution", GameOptions.Resolution, settings.Resolution, (s, i) => s with { Resolution = i }),
                    Combo("Frame rate", GameOptions.Fps, settings.Fps, (s, i) => s with { Fps = i }),
                    Combo("Display", GameOptions.Display, settings.Display, (s, i) => s with { Display = i }),
                    Combo("Anti-aliasing", GameOptions.AntiAliasing, settings.AntiAliasing, (s, i) => s with { AntiAliasing = i }),
                    Combo("HUD updates", GameOptions.HudUpdates, settings.HudUpdates, (s, i) => s with { HudUpdates = i }),
                    Combo("Graphics API", GameOptions.GraphicsApi, settings.GraphicsApi, (s, i) => s with { GraphicsApi = i })),
                Subtitle("Game"),
                Pairs(
                    Combo("Intro", GameOptions.Intro, settings.Intro, (s, i) => s with { Intro = i }),
                    Combo("Button prompts", GameOptions.Prompts, settings.Prompts, (s, i) => s with { Prompts = i })),
                TextBlock("\"Game setting\" keeps what you chose in the game's pause menu. Any other choice here replaces it every time the game starts.")
                    .TextWrapping().Foreground(Theme.SecondaryText),
                Expander("Advanced",
                    VStack(16,
                        TextBox(settings.ExtraArguments, v => updateSettings(s => s with { ExtraArguments = v }),
                                placeholderText: "for example --log_file=run.log", header: "More mcla.exe options"),
                        VStack(4,
                            CheckBox(settings.CheckForUpdates, v => updateSettings(s => s with { CheckForUpdates = v }), "Check for updates when the launcher opens"),
                            CheckBox(settings.CloseOnLaunch, v => updateSettings(s => s with { CloseOnLaunch = v }), "Close the launcher when the game starts"),
                            CheckBox(settings.AutoPlay, v => updateSettings(s => s with { AutoPlay = v }), "Start the game right away when there's nothing to update")))
                ).HAlign(HorizontalAlignment.Stretch).HorizontalContentAlignment(HorizontalAlignment.Stretch)
            ).Padding(0, 0, 12, 0)).Margin(0, 16, 0, 0);

        Element Actions()
        {
            var buttons = new List<Element>();
            if (busy)
                buttons.Add(Button("Stop build", () => buildRef.Current?.Stop()));
            else if (status != null && error == null)
            {
                if (status.UpdateAvailable)
                    buttons.Add(Button("Update and rebuild", () => StartBuild(BuildKind.Update)).AccentButton());
                else if (!status.GameBuilt)
                    buttons.Add(Button("Build the game", () => StartBuild(BuildKind.Build)).AccentButton());
                else if (!status.BuildCurrent || failed)
                    buttons.Add(Button("Rebuild", () => StartBuild(BuildKind.Build)).AccentButton());
            }

            var play = Button(HStack(8, Icon(Symbol.Play), TextBlock(gameRunning ? "Running" : "Play")), Play)
                .AutomationName("Play");
            if (buttons.Count == 0 && !busy) play = play.AccentButton();
            buttons.Add(play.IsEnabled(gameBuilt && !busy && !gameRunning)
                            .ToolTip(status?.UpdateAvailable == true ? "Play the installed version without updating" : "Start the game")
                            .MinWidth(120));
            return HStack(8, buttons.ToArray()).HAlign(HorizontalAlignment.Right).Margin(0, 16, 0, 0);
        }

        var exitDialog = ContentDialog("Stop the build?",
            TextBlock("A build is running. Quitting stops it; finished steps are kept, so the next build continues where this one stopped.").TextWrapping(),
            "Stop and quit") with
        {
            IsOpen = confirmExit,
            CloseButtonText = "Keep building",
            DefaultButton = ContentDialogButton.Close,
            OnClosed = result =>
            {
                setConfirmExit(false);
                if (result != ContentDialogResult.Primary) return;
                exitingRef.Current = true;
                buildRef.Current?.Stop();
                ReactorApp.Exit();
            },
        };

        return Grid(
            columns: [GridSize.Star()],
            rows: [GridSize.Auto, GridSize.Auto, GridSize.Star(), GridSize.Auto, GridSize.Auto],
            Header().Grid(row: 0),
            VStack(8, StatusBar(), WhatsNew(), LauncherUpdateBar(), CountdownBar(), NoticeBar()).Margin(0, 16, 0, 0).Grid(row: 1),
            MainPane().Grid(row: 2),
            ActivityRow().Grid(row: 3),
            Actions().Grid(row: 4),
            exitDialog
        ).Padding(24);
    }
}

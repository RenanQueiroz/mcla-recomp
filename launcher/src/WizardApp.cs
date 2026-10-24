// The setup wizard: the launcher's first run, for a PC with no install. Welcome (the folder), Pick your game,
// Checking your game, Setting up (six steps, one bar each), Ready. The work is done by SetupEngine
// (Setup\SetupEngine.cs); this draws it. See DESIGN section 1 and launcher\README.md ("The setup wizard").
using System.Collections.Immutable;
using System.Diagnostics;
using Microsoft.UI.Reactor;
using Microsoft.UI.Reactor.Core;
using Microsoft.UI.Reactor.Hooks;
using Microsoft.UI.Reactor.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;
using MclaLauncher.Setup;
using static Microsoft.UI.Reactor.Factories;

namespace MclaLauncher;

enum WizardPage { Welcome, PickGame, Checking, Setup, Ready }

enum RunState { Idle, Running, Failed, Cancelled, Done }

sealed class WizardApp : Component
{
    const string Mono = "Cascadia Mono, Consolas";
    static readonly string DefaultFolder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MCLA");

    public override Element Render()
    {
        var dev = DevOptions.Current;
        var earlier = UseMemo(() => Paths.InstallRoot is { } root ? InstallRecord.Load(Path.Combine(root, InstallLayout.RecordName)) : null, Array.Empty<object>());
        var (page, setPage) = UseState(WizardPage.Welcome);
        var (installDir, setInstallDir) = UseState(dev.InstallDir ?? earlier?.InstallRoot ?? DefaultFolder);
        var (gamePath, setGamePath) = UseState<string?>(dev.GamePath ?? earlier?.GameSource);
        var (inPlace, setInPlace) = UseState(dev.InPlace || earlier?.GameMode == "in-place");
        var (release, setRelease) = UseState<ReleaseInfo?>(null, threadSafe: true);
        var (releaseSource, setReleaseSource) = UseState<ReleaseSource?>(null, threadSafe: true);
        var (releaseError, setReleaseError) = UseState<string?>(null, threadSafe: true);
        var (check, setCheck) = UseState<GameCheckResult?>(null, threadSafe: true);
        var (checking, setChecking) = UseState(false, threadSafe: true);
        var (runState, setRunState) = UseState(RunState.Idle, threadSafe: true);
        var (tick, bump) = UseReducer(0, threadSafe: true);
        var (notice, setNotice) = UseState<string?>(null, threadSafe: true);
        var (confirmExit, setConfirmExit) = UseState(false);
        var (expanded, setExpanded) = UseState(ImmutableHashSet<StepId>.Empty);
        var engineRef = UseRef<SetupEngine?>(null);
        var ctsRef = UseRef<CancellationTokenSource?>(null);
        var exitingRef = UseRef(false);
        var autoRef = UseRef(dev.Auto);

        // ---- the release: GitHub's latest, or the development override ------------------------------
        async Task LoadReleaseAsync()
        {
            setReleaseError(null);
            setRelease(null);
            try
            {
                var source = ReleaseSource.FromOverride(dev.ReleaseSource);
                setReleaseSource(source);
                setRelease(await Task.Run(() => source.LoadAsync(CancellationToken.None)));
            }
            catch (SetupException ex)
            {
                setReleaseError(ex.Plain);
            }
        }
        UseEffect(() =>
        {
            if (ReactorApp.PrimaryWindow?.AppWindow is { } appWindow)
                appWindow.TitleBar.PreferredTheme = Microsoft.UI.Windowing.TitleBarTheme.UseDefaultAppMode;
            _ = LoadReleaseAsync();
        }, Array.Empty<object>());

        // ---- the game ----------------------------------------------------------------------------------
        void StartCheck(string path)
        {
            setGamePath(path);
            setCheck(null);
            setChecking(true);
            setPage(WizardPage.Checking);
            var fingerprint = release!.Game!.ToFingerprint();
            _ = Task.Run(() =>
            {
                var result = GameChecker.Inspect(path, fingerprint, out var source);
                source?.Dispose();
                setCheck(result);
                setChecking(false);
            });
        }

        async void ChooseDisc()
        {
            var picking = UseFilePickerAsync(new FilePickerOptions(FileTypeFilter: [".iso"], CommitButtonText: "Choose"));
            try
            {
                var file = await picking;
                if (file != null) StartCheck(file.Path);
            }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException) { setNotice($"Couldn't open the file chooser: {ex.Message}"); }
        }

        async void ChooseGameFolder()
        {
            var picking = UseFolderPickerAsync(new FolderPickerOptions(CommitButtonText: "Choose"));
            try
            {
                var folder = await picking;
                if (folder != null) StartCheck(folder.Path);
            }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException) { setNotice($"Couldn't open the folder chooser: {ex.Message}"); }
        }

        async void ChooseInstallFolder()
        {
            var picking = UseFolderPickerAsync(new FolderPickerOptions(CommitButtonText: "Choose"));
            try
            {
                var folder = await picking;
                if (folder != null) setInstallDir(folder.Path);
            }
            catch (Exception ex) when (ex is System.Runtime.InteropServices.COMException or InvalidOperationException) { setNotice($"Couldn't open the folder chooser: {ex.Message}"); }
        }

        async Task DroppedAsync(DragData data)
        {
            if (release == null) return;
            var items = await data.GetFilesAsync();
            if (items.Count > 0) StartCheck(items[0].Path);
        }

        // ---- the setup ---------------------------------------------------------------------------------
        void StartSetup()
        {
            if (release == null || releaseSource == null || gamePath == null || runState == RunState.Running) return;
            var root = Path.GetFullPath(installDir.Trim());
            var engine = new SetupEngine(new SetupContext(new InstallLayout(root), releaseSource, release,
                new SetupChoices(root, gamePath, inPlace && check?.Ok == true && Directory.Exists(gamePath), dev.GameArguments),
                AppContext.BaseDirectory));
            engine.Changed += () => bump(t => t + 1);
            engineRef.Current = engine;
            var cts = new CancellationTokenSource();
            ctsRef.Current = cts;
            setNotice(null);
            setRunState(RunState.Running);
            setPage(WizardPage.Setup);
            _ = Task.Run(async () =>
            {
                try
                {
                    await engine.RunAsync(cts.Token);
                    setRunState(RunState.Done);
                    setPage(WizardPage.Ready);
                }
                catch (OperationCanceledException) { setRunState(RunState.Cancelled); }
                catch (Exception) { setRunState(RunState.Failed); }
                bump(t => t + 1);
            });
        }

        void CancelSetup()
        {
            ctsRef.Current?.Cancel();
            engineRef.Current?.Cancel();
        }

        void OpenLauncher()
        {
            try
            {
                Process.Start(new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false, WorkingDirectory = AppContext.BaseDirectory });
                exitingRef.Current = true;
                ReactorApp.Exit();
            }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
            {
                setNotice($"Couldn't open the launcher: {ex.Message}");
            }
        }

        void Open(string target)
        {
            try { Process.Start(new ProcessStartInfo(target) { UseShellExecute = true }); }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or IOException) { setNotice($"Couldn't open {target}: {ex.Message}"); }
        }

        void CopyLog()
        {
            try
            {
                var engine = engineRef.Current;
                var text = new System.Text.StringBuilder();
                text.AppendLine($"Midnight Club: Los Angeles setup log ({DateTime.Now:yyyy-MM-dd HH:mm:ss})");
                text.AppendLine($"release {release?.Tag} {release?.Commit}; install {installDir}");
                if (engine?.Failure != null) text.AppendLine($"FAILED: {engine.Failure}");
                if (engine?.FailureDetail != null) text.AppendLine(engine.FailureDetail);
                if (engine?.LogFilePath != null && File.Exists(engine.LogFilePath))
                {
                    using var stream = new FileStream(engine.LogFilePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
                    using var reader = new StreamReader(stream);
                    var lines = reader.ReadToEnd().Split('\n');
                    text.AppendLine(string.Join('\n', lines.Skip(Math.Max(0, lines.Length - 400))));
                }
                var package = new Windows.ApplicationModel.DataTransfer.DataPackage();
                package.SetText(text.ToString());
                Windows.ApplicationModel.DataTransfer.Clipboard.SetContent(package);
                setNotice("The log is copied. Paste it into a report on GitHub.");
            }
            catch (Exception ex) when (ex is IOException or System.Runtime.InteropServices.COMException or UnauthorizedAccessException)
            {
                setNotice($"Couldn't copy the log: {ex.Message}");
            }
        }

        // ---- effects -----------------------------------------------------------------------------------
        // Closing the window mid-setup asks first; quitting stops the setup (what is done is kept).
        UseEffect(() =>
        {
            var window = ReactorApp.PrimaryWindow;
            if (window == null) return () => { };
            void OnClosing(object? sender, WindowClosingEventArgs e)
            {
                if (exitingRef.Current || ctsRef.Current is not { IsCancellationRequested: false } || engineRef.Current == null) return;
                if (runState != RunState.Running) return;
                e.Cancel = true;
                setConfirmExit(true);
            }
            window.Closing += OnClosing;
            return () => window.Closing -= OnClosing;
        }, runState);

        // The development path through the pages: the release, then the check, then the setup, with no clicks.
        UseEffect(() =>
        {
            if (!autoRef.Current || release == null || dev.GamePath == null || page != WizardPage.Welcome) return;
            if (PathBudget.Problem(installDir, out _) != null) return;
            autoRef.Current = false;
            StartCheck(dev.GamePath);
        }, release);
        UseEffect(() =>
        {
            if (dev.Auto && check is { Ok: true } && page == WizardPage.Checking && runState == RunState.Idle) StartSetup();
        }, check);
        UseEffect(() =>
        {
            if (page == WizardPage.Ready && dev.Auto) return;
        }, page);

        // The taskbar button shows the setup's progress.
        UseEffect(() =>
        {
            var progress = ReactorApp.PrimaryWindow?.Progress;
            if (progress == null) return;
            var steps = engineRef.Current?.Steps;
            if (runState == RunState.Running && steps != null)
            {
                progress.State = TaskbarProgressState.Normal;
                progress.Value = Math.Clamp(steps.Sum(s => s.Finished ? 1.0 : s.Fraction ?? 0) / steps.Count, 0, 1);
            }
            else if (runState == RunState.Failed) { progress.State = TaskbarProgressState.Error; progress.Value = 1; }
            else progress.Clear();
        }, runState, tick);

        // Development: close the window after a while (a run nobody watches).
        UseEffect(() =>
        {
            if (dev.QuitAfterSeconds <= 0) return () => { };
            var cts = new CancellationTokenSource();
            _ = Task.Delay(TimeSpan.FromSeconds(dev.QuitAfterSeconds), cts.Token).ContinueWith(t =>
            {
                if (t.IsCanceled) return;
                exitingRef.Current = true;
                CancelSetup();
                ReactorApp.UIDispatcher.TryEnqueue(() => ReactorApp.Exit());
            }, TaskScheduler.Default);
            return () => cts.Cancel();
        }, Array.Empty<object>());

        // Development: a PNG of the window at each page and state.
        UseEffect(() => { _ = WizardShots.TakeAsync($"{page}-{runState}"); }, page, runState, check == null, release == null, checking);

        // ---- layout ------------------------------------------------------------------------------------
        long needed = release?.Game is { } g ? PathBudget.RequiredBytes(g.Files.Sum(f => f.Size), !inPlace) : 0;
        string? folderProblem = PathBudget.Problem(installDir, out var resumes);
        string? folderWarning = folderProblem == null ? PathBudget.Warning(installDir) : null;
        long free = PathBudget.FreeBytes(installDir);
        bool enoughRoom = resumes || free >= needed;

        static Element Info(string title, string? message, InfoBarSeverity severity) =>
            (InfoBar(title, message) with { IsOpen = true, IsClosable = false }).Severity(severity);

        Element DevBanner() => dev.Any
            ? Info("Development options are in use", dev.Banner(), InfoBarSeverity.Warning)
            : Empty();

        Element Welcome()
        {
            var layout = new InstallLayout(Path.GetFullPath(string.IsNullOrWhiteSpace(installDir) ? DefaultFolder : installDir.Trim()));
            return VStack(14,
                VStack(2, Title("Midnight Club: Los Angeles"), TextBlock("Setting up your PC").Foreground(Theme.SecondaryText)),
                TextBlock("This sets up Midnight Club: Los Angeles for PC. You need your own copy of the game: the disc image of the Complete Edition (USA, Europe). "
                          + "The game is translated and built on this PC from your own files, which takes about 5 to 30 minutes, and needs "
                          + (release == null ? "about 9 GB" : $"about {needed / (double)(1L << 30):0} GB") + " of disk space besides the disc image.")
                    .TextWrapping(),
                VStack(6,
                    BodyStrong("Where to put it"),
                    Grid(
                        columns: [GridSize.Star(), GridSize.Auto],
                        rows: [GridSize.Auto],
                        TextBox(installDir, setInstallDir).AutomationName("Install folder").Grid(column: 0),
                        Button("Browse…", ChooseInstallFolder).Margin(8, 0, 0, 0).Grid(column: 1)),
                    TextBlock(release == null ? "" : $"{GameChecker.FormatSize(free)} free on this drive; the setup needs about {GameChecker.FormatSize(needed)}.")
                        .Foreground(enoughRoom ? Theme.SecondaryText : Theme.Ref("SystemFillColorCriticalBrush"))),
                folderProblem != null ? Info("This folder won't do", folderProblem, InfoBarSeverity.Error) : Empty(),
                folderWarning != null ? Info("A better folder exists", folderWarning, InfoBarSeverity.Warning) : Empty(),
                resumes ? Info("An earlier setup is in this folder", "It continues where it stopped; what was done is kept.", InfoBarSeverity.Informational) : Empty(),
                !enoughRoom ? Info("Not enough room", $"There is {GameChecker.FormatSize(free)} free on that drive. Free some space or choose another drive.", InfoBarSeverity.Error) : Empty(),
                releaseError != null
                    ? (InfoBar("Couldn't find the latest release", releaseError) with { IsOpen = true, IsClosable = false, ActionButtonContent = "Try again", OnActionButtonClick = () => _ = LoadReleaseAsync() }).Severity(InfoBarSeverity.Error)
                    : release == null ? HStack(8, ProgressRing().Width(20).Height(20), TextBlock("Looking up the latest release…").Foreground(Theme.SecondaryText))
                    : TextBlock($"Release {release.Tag}" + (releaseSource?.IsOverride == true ? "  (from a local release source)" : "")).Foreground(Theme.SecondaryText),
                DevBanner());
        }

        Element GameZone() =>
            Border(VStack(10,
                    Icon(Symbol.OpenFile).HAlign(HorizontalAlignment.Center),
                    TextBlock("Drop the disc image (.iso) or the folder with default.xex here").HAlign(HorizontalAlignment.Center),
                    HStack(8, Button("Choose the disc image…", ChooseDisc).AccentButton(), Button("Choose a folder…", ChooseGameFolder))
                        .HAlign(HorizontalAlignment.Center))
                .Padding(24))
            .BorderThickness(1).CornerRadius(8)
            .BorderBrush(Theme.CardStroke).Background(Theme.CardBackground)
            .OnDragOver(args => { args.AcceptedOperation = DragOperations.Copy; args.UIOverride.Caption = "Use this"; })
            .OnDrop(args => { _ = DroppedAsync(args.Data); });

        Element PickGame() =>
            VStack(14,
                VStack(2, Title("Pick your game"), TextBlock("Your own copy of the game, as a disc image or as extracted files").Foreground(Theme.SecondaryText)),
                TextBlock("Choose the .iso of your Midnight Club: Los Angeles Complete Edition disc, or the folder its files were extracted into (the one that has default.xex in it). "
                          + "Nothing is changed in your copy: the setup reads it and copies it into the install.").TextWrapping(),
                GameZone(),
                gamePath != null ? TextBlock($"Chosen: {gamePath}").Foreground(Theme.SecondaryText).IsTextSelectionEnabled() : Empty(),
                DevBanner());

        Element Checking()
        {
            if (checking || check == null)
                return VStack(14, Title("Checking your game"),
                    HStack(10, ProgressRing().Width(24).Height(24), TextBlock($"Reading {gamePath}…")), DevBanner());
            var result = check;
            var folder = gamePath != null && Directory.Exists(gamePath);
            return VStack(14,
                Title("Checking your game"),
                Info(result.Ok ? $"✓  {result.Headline}" : result.Headline, result.Detail, result.Ok ? InfoBarSeverity.Success : InfoBarSeverity.Error),
                result.Files.Count > 0
                    ? Expander(result.Ok ? "The 13 files" : "The files", VStack(2, result.Files.Select(f =>
                        TextBlock($"{(f.Ok ? "✓" : "✕")}  {f.Path}   {(f.Present ? GameChecker.FormatSize(f.Found!.Value) : "missing")}").FontFamily(Mono).FontSize(12)
                            .Foreground(f.Ok ? Theme.PrimaryText : Theme.Ref("SystemFillColorCriticalBrush")).WithKey(f.Path)).ToArray()), isExpanded: !result.Ok)
                    : Empty(),
                result.Ok && folder
                    ? VStack(4,
                        CheckBox(inPlace, setInPlace, "Use the game where it is"),
                        TextBlock("Nothing is copied: the game stays in that folder (it has to stay there), and the game is told where it is. Only default.xex is copied, for the build.")
                            .Foreground(Theme.SecondaryText).TextWrapping())
                    : Empty(),
                DevBanner());
        }

        Element StepRow(StepStatus step)
        {
            var color = step.State switch
            {
                StepState.Failed => Theme.Ref("SystemFillColorCriticalBrush"),
                StepState.Done or StepState.Skipped => Theme.Ref("SystemFillColorSuccessBrush"),
                StepState.Waiting or StepState.Cancelled => Theme.DisabledText,
                _ => Theme.PrimaryText,
            };
            Element glyph = step.State switch
            {
                StepState.Running => ProgressRing().Width(18).Height(18),
                StepState.Done or StepState.Skipped => TextBlock("✓").Foreground(color).FontSize(16),
                StepState.Failed => TextBlock("✕").Foreground(color).FontSize(16),
                _ => TextBlock("○").Foreground(color).FontSize(16),
            };
            var log = engineRef.Current?.LogOf(step.Id) ?? ImmutableList<string>.Empty;
            var open = expanded.Contains(step.Id);
            return VStack(4,
                Grid(
                    columns: [GridSize.Px(30), GridSize.Star(), GridSize.Auto],
                    rows: [GridSize.Auto],
                    glyph.HAlign(HorizontalAlignment.Left).VAlign(VerticalAlignment.Center).Grid(column: 0),
                    VStack(1,
                        BodyStrong(step.Title),
                        TextBlock(step.Text).Foreground(Theme.SecondaryText).TextTrimming(TextTrimming.CharacterEllipsis).MaxLines(1)).Grid(column: 1),
                    TextBlock(step.Remaining ?? "").Foreground(Theme.SecondaryText).VAlign(VerticalAlignment.Center).Grid(column: 2)),
                step.State == StepState.Running
                    ? (step.Fraction is double f ? Progress(f * 100) : ProgressIndeterminate()).Margin(30, 0, 0, 0)
                    : step.Finished ? Progress(100).Margin(30, 0, 0, 0) : Empty(),
                Expander("Details", ScrollViewer(TextBlock(string.Join("\n", log.TakeLast(300))).FontFamily(Mono).FontSize(11).IsTextSelectionEnabled().TextWrapping()).MaxHeight(160),
                         isExpanded: open, onIsExpandedChanged: v => setExpanded(v ? expanded.Add(step.Id) : expanded.Remove(step.Id)))
                    .Margin(30, 2, 0, 0)).WithKey(step.Id.ToString());
        }

        Element SetupPage()
        {
            var engine = engineRef.Current;
            var steps = engine?.Steps ?? [];
            return VStack(12,
                VStack(2, Title("Setting up"),
                    TextBlock(runState switch
                    {
                        RunState.Running => "This takes a few minutes. You can use the PC meanwhile.",
                        RunState.Failed => "The setup stopped.",
                        RunState.Cancelled => "The setup was cancelled. What was done is kept.",
                        _ => "",
                    }).Foreground(Theme.SecondaryText)),
                runState == RunState.Failed && engine?.Failure != null ? Info("Something went wrong", engine.Failure, InfoBarSeverity.Error) : Empty(),
                notice != null ? Info("", notice, InfoBarSeverity.Informational) : Empty(),
                (ScrollViewer(VStack(14, steps.Select(StepRow).ToArray())) with { HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled }).MaxHeight(520),
                DevBanner());
        }

        Element ReadyPage() =>
            VStack(14,
                Title("Your game is ready"),
                TextBlock("Midnight Club: Los Angeles is built on this PC. The launcher opens next, with Play and the game's settings.").TextWrapping(),
                Button(HStack(8, Icon(Symbol.Play), TextBlock("Open the launcher")), OpenLauncher).AccentButton().AutomationName("Open the launcher"),
                DevBanner());

        Element Footer()
        {
            var buttons = new List<Element>();
            switch (page)
            {
                case WizardPage.Welcome:
                    buttons.Add(Button("Next", () => setPage(WizardPage.PickGame)).AccentButton()
                        .IsEnabled(release != null && folderProblem == null && enoughRoom));
                    break;
                case WizardPage.PickGame:
                    buttons.Add(Button("Back", () => setPage(WizardPage.Welcome)));
                    buttons.Add(Button("Next", () => { if (gamePath != null) StartCheck(gamePath); }).AccentButton().IsEnabled(gamePath != null));
                    break;
                case WizardPage.Checking:
                    buttons.Add(Button(check?.Ok == true ? "Back" : "Choose another", () => setPage(WizardPage.PickGame)));
                    if (check?.Ok == true)
                        buttons.Add(Button("Start", StartSetup).AccentButton());
                    break;
                case WizardPage.Setup:
                    if (runState == RunState.Running) buttons.Add(Button("Cancel", CancelSetup));
                    else
                    {
                        if (runState == RunState.Failed) buttons.Add(Button("Copy the log", CopyLog));
                        if (runState == RunState.Failed) buttons.Add(Button("Report it on GitHub", () => Open(Paths.IssuesUrl)));
                        buttons.Add(Button("Back", () => setPage(WizardPage.Checking)));
                        buttons.Add(Button("Try again", StartSetup).AccentButton());
                    }
                    break;
            }
            return HStack(8, buttons.ToArray()).HAlign(HorizontalAlignment.Right).Margin(0, 16, 0, 0);
        }

        Element body = page switch
        {
            WizardPage.Welcome => Welcome(),
            WizardPage.PickGame => PickGame(),
            WizardPage.Checking => Checking(),
            WizardPage.Setup => SetupPage(),
            _ => ReadyPage(),
        };

        var exitDialog = ContentDialog("Stop the setup?",
            TextBlock("The setup is running. Quitting stops it; what is done is kept, and the next start continues from there.").TextWrapping(),
            "Stop and quit") with
        {
            IsOpen = confirmExit,
            CloseButtonText = "Keep going",
            DefaultButton = ContentDialogButton.Close,
            OnClosed = result =>
            {
                setConfirmExit(false);
                if (result != ContentDialogResult.Primary) return;
                exitingRef.Current = true;
                CancelSetup();
                ReactorApp.Exit();
            },
        };

        var root = Grid(
            columns: [GridSize.Star()],
            rows: [GridSize.Star(), GridSize.Auto],
            (ScrollViewer(body.Padding(0, 0, 12, 0)) with { HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled }).Grid(row: 0),
            Footer().Grid(row: 1),
            exitDialog
        ).Padding(24);
        return dev.ShotsDir != null ? root.Background(Theme.Ref("SolidBackgroundFillColorBaseBrush")) : root;
    }
}

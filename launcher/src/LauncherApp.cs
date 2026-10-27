using System.Collections.Immutable;
using System.Diagnostics;
using System.Globalization;
using Microsoft.UI.Reactor;
using Microsoft.UI.Reactor.Core;
using Microsoft.UI.Reactor.Hooks;
using Microsoft.UI.Reactor.Input;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Windows.System;
using MclaLauncher.Setup;
using static Microsoft.UI.Reactor.Factories;
using MenuItemBase = Microsoft.UI.Reactor.Core.MenuFlyoutItemBase;

namespace MclaLauncher;

sealed class LauncherApp : Component
{
    const int MaxLogLines = 2000;
    const string Mono = "Cascadia Mono, Consolas";

    // The game's settings file, next to mcla.exe (GameSettings.cs).
    static readonly string GameSettingsPath = GameSettingsFile.PathIn(Paths.BuildDir);

    static Dictionary<string, string> ReadGameSettings()
    {
        try
        {
            return GameSettingsFile.Read(GameSettingsPath);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Debug.WriteLine($"game settings: {ex.Message}");
            return new();
        }
    }

    static bool GameIsRunning() => GameSettings.GameIsRunning();

    public override Element Render()
    {
        // Loading the settings also moves an older launcher's choices into
        // mcla.toml, once, so it comes before the file is read.
        var initialSettings = UseMemo(LauncherSettings.Load, Array.Empty<object>());
        var (settings, updateSettings) = UseReducer(initialSettings, threadSafe: true);
        var settingsRef = UseRef(initialSettings);
        settingsRef.Current = settings;
        var initialGameSettings = UseMemo(ReadGameSettings, Array.Empty<object>());
        var (gameSettings, setGameSettings) = UseState(initialGameSettings, threadSafe: true);
        var (status, setStatus) = UseState<RepoStatus?>(null);
        var (checking, setChecking) = UseState(true);
        var (error, setError) = UseState<string?>(null);
        var (notice, setNotice) = UseState<string?>(null);
        var (build, setBuild) = UseState<BuildRun?>(null);
        var (buildResult, setBuildResult) = UseState<int?>(null);
        // An install made by the setup wizard (Installed mode) updates from a release (Setup\Updates.cs), and may update itself.
        var installed = Paths.Mode == LaunchMode.Installed;
        // What an Installed launcher starts to build ends with it (the job object), as the setup's builds do.
        if (installed) ChildJob.Enabled = true;
        var (updating, setUpdating) = UseState<string?>(null, threadSafe: true);
        var updateCtsRef = UseRef<CancellationTokenSource?>(null);
        var (channel, setChannel) = UseState(installed ? InstallChannel.Read(Paths.InstallRoot!) : InstallChannel.Release);
        var (log, updateLog) = UseReducer(ImmutableList<string>.Empty, threadSafe: true);
        var (step, setStep) = UseState<string?>(null);
        var (activity, setActivity) = UseState<Activity?>(null);
        // Preparing shaders (ShaderPrep.cs): after a build, or from More.
        var (prep, setPrep) = UseState<ShaderPrepRun?>(null, threadSafe: true);
        var (prepProgress, setPrepProgress) = UseState<ShaderPrepProgress?>(null, threadSafe: true);
        var (prepResult, setPrepResult) = UseState<int?>(null, threadSafe: true);
        var prepRef = UseRef<ShaderPrepRun?>(null);
        // The game starts once the preparation that runs before it ends (PrepareKind.BeforePlay,
        // or the player's Play now).
        var playAfterPrepRef = UseRef(false);
        // The player ended a preparation with Stop: Play doesn't ask for another until the
        // launcher is opened again or a preparation is asked for.
        var prepDeclinedRef = UseRef(false);
        var initialRunning = UseMemo(GameIsRunning, Array.Empty<object>());
        var (gameRunning, setGameRunning) = UseState(initialRunning, threadSafe: true);
        var runningRef = UseRef(initialRunning);
        runningRef.Current = gameRunning;
        var (countdown, setCountdown) = UseState<int?>(null);
        // An HDR level slider being dragged: its value until the drag ends.
        var (levelDrafts, setLevelDrafts) = UseState(ImmutableDictionary<string, double>.Empty);
        // What was typed in the Player name box (null: show the file's), spaces and all.
        var (nameDraft, setNameDraft) = UseState<string?>(null);
        var (confirmExit, setConfirmExit) = UseState(false);
        var buildRef = UseRef<BuildRun?>(null);
        var exitingRef = UseRef(false);
        var autoPlayDoneRef = UseRef(false);
        var followLogRef = UseRef(true);
        var logScroller = this.UseElementRef<ScrollViewer>();
        var logText = UseMemo(() => string.Join("\n", log), log);

        var busy = (build != null && buildResult == null) || updating != null;
        var preparing = prep != null && prepResult == null;
        var failed = buildResult is int exit && exit != 0 && build?.StopRequested != true;
        var gameBuilt = status?.GameBuilt ?? File.Exists(Paths.GameExe);
        var readyToPlay = status is { GameBuilt: true, BuildCurrent: true, UpdateAvailable: false }
                          && !checking && !busy && !preparing && build == null && error == null;

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

        // Adds what the shader storage lacks of the shader pack and compiles every stored
        // pipeline, with the options the game is started with (ShaderPrep.cs). Quietly does
        // nothing in a version without a pack. Returns whether it started.
        bool StartPrepare(PrepareKind kind, string? reason = null)
        {
            if (prepRef.Current is { Completion.IsCompleted: false }) return false;
            var manual = kind == PrepareKind.Manual;
            // The pack the game will use: a custom one given with --mcla_shader_pack, or the
            // fork's. A path the game can't use either (a bad character) is none.
            string? packFile = null;
            try { packFile = ShaderStamp.PackFile(Paths.Repo, GameOptions.Arguments(settingsRef.Current), ReadGameSettings(), Paths.BuildDir); }
            catch (Exception ex) when (ex is ArgumentException or NotSupportedException) { }
            if (packFile == null || !File.Exists(packFile) || !File.Exists(Paths.GameExe))
            {
                if (manual) setNotice("This version has no shader pack, so there are no shaders to prepare.");
                return false;
            }
            if (GameIsRunning())
            {
                if (manual) setNotice("Close the game first: shaders are prepared while it isn't running.");
                return false;
            }
            // Nothing else starts the game meanwhile.
            setCountdown(null);
            setPrepResult(null);
            setPrepProgress(null);
            prepDeclinedRef.Current = false;
            try
            {
                var run = new ShaderPrepRun(Paths.GameExe, GameOptions.Arguments(settingsRef.Current), setPrepProgress, Paths.LogsDir,
                                            reason, retry: manual);
                var pack = ShaderStamp.PackHash(packFile);
                prepRef.Current = run;
                playAfterPrepRef.Current = kind == PrepareKind.BeforePlay;
                setPrep(run);
                _ = run.Completion.ContinueWith(task =>
                {
                    var exit = task.IsFaulted ? -1 : task.Result;
                    // A run the player (or the launcher closing) ended says nothing about the
                    // pack. A failure for good (not "in use") isn't tried again before every Play
                    // for the same pack, whoever asked for the run; only a success clears that.
                    if (!run.StopRequested && exit == 0)
                    {
                        updateSettings(s => s.ShaderPrepSkipPack == null ? s : s with { ShaderPrepSkipPack = null });
                    }
                    else if (!run.StopRequested && !ShaderPrepRun.IsTransient(exit) && pack != null)
                    {
                        updateSettings(s => s with { ShaderPrepSkipPack = pack });
                    }
                    setPrepResult(exit);
                }, TaskScheduler.Default);
                return true;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            {
                if (manual) setNotice($"Couldn't prepare shaders: {ex.Message}");
                return false;
            }
        }

        // An install made by the setup: the update is a release's (or main's) tag, fetched and checked out, with the tools and
        // the SDK bundle it names downloaded first (Setup\Updates.cs); then the build runs as for any update.
        async void StartInstalledUpdate()
        {
            if (Process.GetProcessesByName("mcla").Length > 0)
            {
                setNotice("Close the game first: the build can't replace mcla.exe while it's running.");
                return;
            }
            if (status?.Target is not { } target)
            {
                setNotice("Check for updates first.");
                return;
            }
            setNotice(null);
            setUpdating("Getting ready…");
            var cts = new CancellationTokenSource();
            updateCtsRef.Current = cts;
            UpdateSession session;
            try
            {
                session = await Task.Run(() => UpdateEngine.PrepareAsync(new InstallLayout(Paths.InstallRoot!), target, line => Debug.WriteLine($"update: {line}"),
                                                                         new Progress<string>(text => setUpdating(text + "…")), cts.Token));
            }
            catch (OperationCanceledException)
            {
                setUpdating(null);
                setNotice("The update was stopped. What was downloaded is kept.");
                return;
            }
            catch (Exception ex)
            {
                // Whatever it is (a full disk, a scanner holding a file, git too slow): said in the window, and the update is
                // over (PrepareAsync has let the folder's lock go). From an async void, an uncaught one would end the launcher.
                setUpdating(null);
                setNotice(ex is SetupException setup ? setup.Plain : $"The update stopped: {ex.Message}");
                return;
            }
            setUpdating(null);
            StartBuild(BuildKind.Build, session.Options, session);
        }

        void StartBuild(BuildKind kind, BuildRunOptions? options = null, UpdateSession? session = null)
        {
            if (installed && kind == BuildKind.Update && options == null)
            {
                StartInstalledUpdate();
                return;
            }
            if (Process.GetProcessesByName("mcla").Length > 0)
            {
                session?.Dispose();
                setNotice("Close the game first: the build can't replace mcla.exe while it's running.");
                return;
            }
            // An update holds the folder's lock through its session; a plain build of an install takes it here, so that it never
            // overlaps a setup or an update (of another window) in the same folder.
            InstallLock? plainLock = null;
            if (installed && session == null)
            {
                try { plainLock = InstallLock.Acquire(Paths.InstallRoot!); }
                catch (SetupException ex) { setNotice(ex.Plain); return; }
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
                }, options);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
            {
                session?.Dispose();
                plainLock?.Dispose();
                setNotice($"Couldn't start the build: {ex.Message}");
                return;
            }
            buildRef.Current = run;
            setBuild(run);
            _ = run.Completion.ContinueWith(task =>
            {
                if (installed)
                {
                    var layout = new InstallLayout(Paths.InstallRoot!);
                    try
                    {
                        // A build that worked finishes an update (its session, or the one a closed launcher left pending).
                        if (task.Result == 0 && !run.StopRequested)
                        {
                            if (session != null) UpdateEngine.Finish(layout, AppContext.BaseDirectory, session);
                            else UpdateEngine.CompletePending(layout, AppContext.BaseDirectory);
                        }
                    }
                    catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { Debug.WriteLine($"update: {ex.Message}"); }
                    finally { session?.Dispose(); plainLock?.Dispose(); }
                }
                setBuildResult(task.Result);
                _ = CheckAsync(fetch: false);
                // After an update the shaders are prepared again only when something the game is made of changed.
                if (task.Result == 0 && !run.StopRequested && (session == null || session.GameChanged)) StartPrepare(PrepareKind.AfterBuild);
            }, TaskScheduler.Default);
        }

        // Play: first, when the stamp says the driver, the settings or the storage changed since
        // the shaders were last prepared, prepare them (with a line saying why), then start.
        void Play()
        {
            setCountdown(null);
            // Never beside a preparation (the countdown included).
            if (prepRef.Current is { Completion.IsCompleted: false }) return;
            if (!prepDeclinedRef.Current)
            {
                var check = ShaderStamp.Check(Paths.Repo, Paths.BuildDir, GameOptions.Arguments(settings), ReadGameSettings(), settings.ShaderPrepSkipPack);
                Debug.WriteLine($"shader check: {check.Reason ?? "nothing to prepare"} ({check.Milliseconds} ms)");
                if (check.Reason != null && StartPrepare(PrepareKind.BeforePlay, check.Reason)) return;
            }
            StartGame();
        }

        // A preparation that runs (before Play, after a build, or asked for) can be ended: Stop
        // ends it and starts nothing, Play now ends it and starts the game. Both are safe: the
        // storage is only ever replaced whole, and the game adds the shader pack and creates the
        // stored pipelines by itself at its start (what it hadn't got to compiles as it meets it).
        void StopPrepare()
        {
            playAfterPrepRef.Current = false;
            prepDeclinedRef.Current = true;
            prepRef.Current?.Stop();
        }

        void PlayNow()
        {
            playAfterPrepRef.Current = true;
            prepRef.Current?.Stop();
        }

        void StartGame()
        {
            setCountdown(null);
            // Not while the launcher is closing (a preparation ending because of it must not
            // start the game).
            if (exitingRef.Current) return;
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
                setGameRunning(true);  // and the check below notices when it closes
            }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
            {
                setNotice($"Couldn't start the game: {ex.Message}");
            }
        }

        // A setting changed here goes into mcla.toml at once, as the pause menu
        // writes it. Not while the game runs: it reads the file only when it
        // starts, and the pause menu is where its settings change then.
        void SetGameSetting(string key, object value)
        {
            if (GameIsRunning())
            {
                setGameRunning(true);
                return;
            }
            try
            {
                Directory.CreateDirectory(Paths.BuildDir);
                GameSettingsFile.Set(GameSettingsPath, new Dictionary<string, object> { [key] = value });
                setGameSettings(ReadGameSettings());
                // A setting chosen here is the user's now: an older launcher's
                // choices that weren't moved yet must never come over it.
                if (!settingsRef.Current.SettingsInGameFile) updateSettings(SettingsMigration.Retire);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
            {
                setNotice($"Couldn't save the setting in {GameSettingsPath}: {ex.Message}");
            }
        }

        // The player's name: what was typed is cleaned (PlayerName.Clean) and shown at once; the name
        // goes into mcla.toml as it is typed, as a drop-down's choice does, and an empty box takes the
        // line out (the game then says "User"). Not while the game runs, as for the other settings.
        void SetPlayerName(string typed)
        {
            var clean = PlayerName.Clean(typed);
            var name = PlayerName.ForFile(clean);
            var inFile = PlayerName.In(gameSettings);
            if (GameIsRunning())
            {
                setGameRunning(true);
                return;
            }
            setNameDraft(clean);
            if (name == (inFile.Length == 0 ? null : inFile)) return;
            if (name is not null)
            {
                SetGameSetting(PlayerName.Key, name);
                return;
            }
            try
            {
                GameSettingsFile.Remove(GameSettingsPath, PlayerName.Key);
                setGameSettings(ReadGameSettings());
                if (!settingsRef.Current.SettingsInGameFile) updateSettings(SettingsMigration.Retire);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
            {
                setNotice($"Couldn't save the setting in {GameSettingsPath}: {ex.Message}");
            }
        }

        // An HDR level: a step, or the end of a drag, goes into mcla.toml (a
        // whole number of nits unless set by hand otherwise; 0 is Auto).
        void SetLevel(LevelSetting level, double value)
        {
            setLevelDrafts(levelDrafts.Remove(level.Key));
            if (Math.Abs(value - level.Value(gameSettings)) < 1e-9 && gameSettings.ContainsKey(level.Key)) return;
            var rounded = Math.Round(value);
            SetGameSetting(level.Key, Math.Abs(value - rounded) < 1e-9 ? (object)(long)rounded : value);
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
            Paths.AddGitDir(psi);
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
                Paths.AddGitDir(psi);
                Process.Start(psi);
                exitingRef.Current = true;
                playAfterPrepRef.Current = false;
                prepRef.Current?.Stop();
                ReactorApp.Exit();
            }
            catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or InvalidOperationException)
            {
                setNotice($"Couldn't update the launcher: {ex.Message}");
            }
        }

        // The launcher of an install made by the setup replacing itself (Setup\LauncherUpdate.cs): the release's MCLA-Setup.exe is
        // fetched and checked (SHA-256 and size from release.json), started as the updater, and this launcher quits; the
        // installer swaps the folders and starts the new launcher, which writes the ready file five seconds after it is up, or puts
        // this one back. Nothing is changed until the installer has the new launcher unpacked and this process has ended.
        async void StartLauncherUpdate(ReleaseInfo release, ReleaseSource source, LauncherUpdate.Way way)
        {
            setNotice(null);
            setUpdating("Getting the new launcher…");
            try
            {
                var progress = new Action<long, long>((done, total) => setUpdating($"Downloading the new launcher: {done / 1_000_000} of {total / 1_000_000} MB…"));
                var setupExe = await Task.Run(() => LauncherUpdate.FetchInstallerAsync(release, source, progress, CancellationToken.None));
                setUpdating("Starting the installer…");
                LauncherUpdate.Start(way, setupExe, LauncherUpdate.NewReadyFile());
            }
            catch (Exception ex)
            {
                setUpdating(null);
                setNotice(ex is SetupException setup ? setup.Plain : $"The launcher could not be updated: {ex.Message}");
                return;
            }
            exitingRef.Current = true;
            playAfterPrepRef.Current = false;
            prepRef.Current?.Stop();
            ReactorApp.Exit();
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
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                setNotice($"Couldn't save the launcher's settings in {Paths.SettingsFile}: {ex.Message}");
            }
        }, settings);

        // Closing the window mid-build asks first; quitting stops the build.
        UseEffect(() =>
        {
            var window = ReactorApp.PrimaryWindow;
            if (window == null) return () => { };
            void OnClosing(object? sender, WindowClosingEventArgs e)
            {
                if (exitingRef.Current || buildRef.Current is not { Completion.IsCompleted: false })
                {
                    // Closing: preparing shaders just stops. The storage is only ever replaced
                    // whole, and the game's next start undoes an unfinished import. The game
                    // doesn't start because of it.
                    exitingRef.Current = true;
                    playAfterPrepRef.Current = false;
                    prepRef.Current?.Stop();
                    return;
                }
                e.Cancel = true;
                setConfirmExit(true);
            }
            window.Closing += OnClosing;
            return () => window.Closing -= OnClosing;
        }, Array.Empty<object>());

        // Whether the game is running, every two seconds. While it runs the
        // settings are its pause menu's to change; when it closes the file is
        // read again, to show what was changed there.
        UseEffect(() =>
        {
            var cts = new CancellationTokenSource();
            _ = Task.Run(async () =>
            {
                using var timer = new PeriodicTimer(TimeSpan.FromSeconds(2));
                try
                {
                    while (await timer.WaitForNextTickAsync(cts.Token))
                    {
                        // Against what is shown, so that a game that started and
                        // closed between two checks doesn't leave it greyed out.
                        var running = GameIsRunning();
                        if (running == runningRef.Current) continue;
                        runningRef.Current = running;
                        setGameRunning(running);
                        if (running) continue;
                        // An older launcher's choices are moved into mcla.toml
                        // only while no game runs (LauncherSettings.Load).
                        if (!settingsRef.Current.SettingsInGameFile &&
                            SettingsMigration.Run(settingsRef.Current, Paths.BuildDir, GameIsRunning).SettingsInGameFile)
                            updateSettings(SettingsMigration.Retire);
                        setGameSettings(ReadGameSettings());
                    }
                }
                catch (OperationCanceledException) { }
            });
            return () => cts.Cancel();
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

        // The game starts once the preparation Play asked for has ended, whatever its result: the
        // game plays either way.
        UseEffect(() =>
        {
            if (prepResult is null || !playAfterPrepRef.Current) return;
            playAfterPrepRef.Current = false;
            StartGame();
        }, prepResult);

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
            if (installed && head != null && InstallRecord.Load(Path.Combine(Paths.InstallRoot!, InstallLayout.RecordName))?.ReleaseTag is { Length: > 0 } releaseTag)
                subtitle += $"  ·  {releaseTag}";
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
                   .. (installed
                       ? (MenuItemBase[])[MenuItem(channel == InstallChannel.Main ? "Follow the releases again" : "Latest changes (main, untested)", SwitchChannel)]
                       : []),
                   MenuItem("Rebuild the game", () => StartBuild(BuildKind.Build)),
                   .. (installed ? [] : (MenuItemBase[])[MenuItem("Re-patch and rebuild the SDK", () => StartBuild(BuildKind.ResetSdk))]),
                   MenuItem("Run the build in a terminal", BuildInTerminal),
                   MenuItem("Prepare shaders", () => StartPrepare(PrepareKind.Manual)),
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
            if (updating != null)
                return Info("Updating", updating, InfoBarSeverity.Informational);
            if (busy)
                return Info(build!.IsUpdate ? "Updating and rebuilding" : "Building",
                            step is null ? "Starting…" : $"Step {step}", InfoBarSeverity.Informational);
            if (build?.StopRequested == true)
                return Info("Build stopped", "Finished steps are kept, so the next build continues where this one stopped.", InfoBarSeverity.Warning);
            if (failed)
                return Info("The build failed", "The output below shows where. Play still starts the last version that built.", InfoBarSeverity.Error);
            if (preparing)
                return Info(prep?.Reason is string why ? $"Preparing shaders: {why}" : "Preparing shaders",
                            (prepProgress?.Describe() ?? "Starting…") + (playAfterPrepRef.Current ? ". The game starts when it's done." : ""),
                            InfoBarSeverity.Informational);
            if (checking || status == null)
                return Empty();
            if (installed && status.Target is { } target && status.UpdateAvailable)
            {
                if (target.IsMain && status.PatchesChanged)
                    return Info("The latest changes can't be installed",
                                "They change the ReXGlue SDK, and an install made by the setup has the SDK that came with its release (it can't build one). "
                                + "More > Follow the releases again goes back to the release; the next release will have the new SDK.", InfoBarSeverity.Warning);
                if (target.SwitchBack)
                    return Info($"You're on newer code than {target.Label}",
                                "Going back checks out that release and builds it again.", InfoBarSeverity.Informational);
                return Info(target.IsMain ? $"The latest changes (main, untested) have {status.Incoming.Count} new change{(status.Incoming.Count == 1 ? "" : "s")}"
                                          : $"An update is available: {target.Label}",
                            "Updating downloads what is new (and, for a release, the SDK and tools it names when they changed) and rebuilds the game, "
                            + "which takes a few minutes." + (target.IsMain ? " These changes are not tested." : "")
                            + (status.PatchesChanged && !target.IsMain ? " This release has a new SDK." : ""),
                            InfoBarSeverity.Informational);
            }
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
            if (prep is { StopRequested: true } && prepResult != null)
                return Info("Shader preparation stopped", "The game still plays: it adds the shader pack by itself and compiles shaders as it meets them. "
                            + "More > Prepare shaders runs it again.", InfoBarSeverity.Informational);
            if (prep != null && prepResult is int prepExit)
                return prepExit == 0
                    ? Info(ShaderPrepRun.Describe(0, null), (prep.PipelinesCreated > 0
                           ? $"{prep.PipelinesCreated:N0} pipelines are compiled for the current settings."
                           : "The shader storage is ready.")
                           + (prep.ImportDisabled ? " The shader pack wasn't added: it stopped the game's start twice. More > Prepare shaders tries again." : ""),
                           prep.ImportDisabled ? InfoBarSeverity.Warning : InfoBarSeverity.Success)
                    : Info("Shaders weren't prepared", ShaderPrepRun.Describe(prepExit, prep.ErrorMessage)
                           + ". The game still plays, compiling shaders as it meets them.", InfoBarSeverity.Warning);
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

        Element? LauncherUpdateBar()
        {
            if (status is not { LauncherOutdated: true } || busy || preparing) return null;
            if (installed && status.Target?.Release is { Launcher: not null } release)
            {
                // What the button does depends on how this launcher got where it is (Setup\LauncherUpdate.cs): MCLA-Setup.exe replaces
                // the folder Setup made, or moves a launcher that was unpacked from a zip into its own folder; with no installer in
                // the release the button opens the release's page. The launcher never swaps its own folder.
                var way = status.Target?.Source == null ? LauncherUpdate.Way.ReleasePage : LauncherUpdate.WayFor(release);
                var megabytes = release.Installer is { } sized ? $"about {Math.Max(1, sized.Size / 1_000_000)} MB" : "";
                var (message, button) = way switch
                {
                    LauncherUpdate.Way.InPlace => ($"The game doesn't need it to play. Updating downloads it ({megabytes}), replaces the launcher's program and opens the new one. "
                                                   + "Your install and settings stay as they are.", "Update the launcher"),
                    LauncherUpdate.Way.Move => ("This launcher was not put here by the setup, so it can't replace itself. Updating installs the new launcher in its own folder "
                                                + $"({megabytes}, with a shortcut) and opens it on the same install; you can delete this folder afterwards. Your install and settings stay as they are.",
                                                "Move to the new launcher"),
                    LauncherUpdate.Way.OtherInstalled => ($"A launcher installed by the setup is already in {LauncherUpdate.SetupFolder}, with an install of its own. Start that one: its update bar "
                                                          + "updates it. This launcher, its install and its settings are not changed.", "Open its folder"),
                    _ => ("The game doesn't need it to play. It is a separate download: the release's page has it. Your install and settings stay as they are.",
                          "Open the release's page"),
                };
                return (InfoBar($"A newer launcher is in {release.Tag}", message) with
                {
                    IsOpen = true,
                    IsClosable = false,
                    ActionButtonContent = button,
                    OnActionButtonClick = way == LauncherUpdate.Way.ReleasePage ? () => Open(LauncherUpdate.ReleasePage(release))
                        : way == LauncherUpdate.Way.OtherInstalled ? () => Open(LauncherUpdate.SetupFolder)
                        : () => StartLauncherUpdate(release, status.Target!.Source!, way),
                }).Severity(InfoBarSeverity.Informational);
            }
            return (InfoBar("The launcher itself was updated", "Restarting rebuilds it (about a minute) and opens the new one.") with
            {
                IsOpen = true,
                IsClosable = false,
                ActionButtonContent = "Restart to update the launcher",
                OnActionButtonClick = RestartToUpdateLauncher,
            }).Severity(InfoBarSeverity.Informational);
        }

        void SwitchChannel()
        {
            var next = channel == InstallChannel.Main ? InstallChannel.Release : InstallChannel.Main;
            try
            {
                InstallChannel.Write(Paths.InstallRoot!, AppContext.BaseDirectory, next);
            }
            catch (Exception ex)
            {
                setNotice(ex is SetupException setup ? setup.Plain : $"Couldn't change what the launcher follows: {ex.Message}");
                return;
            }
            setChannel(next);
            setNotice(next == InstallChannel.Main
                ? "You follow the latest changes (main) now. They are not tested and can be broken. More > Follow the releases again goes back."
                : "You follow the releases again.");
            _ = CheckAsync(fetch: true);
        }

        // The development override of the release source (a folder), when it is in use: said in words, as the wizard does.
        Element? DevBar() => installed && DevOptions.Current.Any
            ? Info("Development options are in use", DevOptions.Current.Banner(), InfoBarSeverity.Warning)
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

        // The "i" next to a setting's title: its tip on hover, and the same
        // text in a flyout on click, Enter or the controller's A, so it
        // doesn't need a mouse.
        static Element InfoTip(string title, string text) =>
            Button(Icon(FontIcon("\uE946", "Segoe Fluent Icons, Segoe MDL2 Assets", 14)))
                .Resources(r => r
                    .Set("ButtonBackground", Theme.Ref("SubtleFillColorTransparentBrush"))
                    .Set("ButtonBackgroundPointerOver", Theme.Ref("SubtleFillColorSecondaryBrush"))
                    .Set("ButtonBackgroundPressed", Theme.Ref("SubtleFillColorTertiaryBrush"))
                    .Set("ButtonBorderBrush", Theme.Ref("SubtleFillColorTransparentBrush"))
                    .Set("ButtonBorderBrushPointerOver", Theme.Ref("SubtleFillColorTransparentBrush"))
                    .Set("ButtonBorderBrushPressed", Theme.Ref("SubtleFillColorTransparentBrush")))
                .Padding(5, 3, 5, 3)
                .VAlign(VerticalAlignment.Center)
                .ToolTip(text)
                .AutomationName($"About {title}")
                .WithFlyout(ContentFlyout(TextBlock(text).TextWrapping().MaxWidth(360)));

        static Element SettingTitle(string title, string info) =>
            HStack(2, TextBlock(title).VAlign(VerticalAlignment.Center), InfoTip(title, info));

        Element SettingCombo(GameSetting setting)
        {
            var (choices, index) = setting.Show(gameSettings);
            return VStack(4,
                SettingTitle(setting.Header, setting.Info),
                ComboBox(choices.Select(c => c.Label).ToArray(), index,
                         i => { if (i >= 0 && i < choices.Length && i != index) SetGameSetting(setting.Key, choices[i].Value); })
                    .AutomationName(setting.Header)
                    .HAlign(HorizontalAlignment.Stretch)
                    .IsEnabled(!gameRunning));
        }

        // The Player name box: the file's name (the placeholder says "User", which the game takes while there
        // is none), or what was typed.
        Element PlayerNameBox() =>
            VStack(4,
                SettingTitle("Player name", PlayerName.Info),
                TextBox(nameDraft ?? PlayerName.In(gameSettings), SetPlayerName, placeholderText: PlayerName.Default)
                    .MaxLength(PlayerName.MaxLength)
                    .AutomationName("Player name")
                    .HAlign(HorizontalAlignment.Stretch)
                    .IsEnabled(!gameRunning));

        // What Auto stands for: what Windows reports for the main display, as
        // the game reads it for its own (1,000 and 200 nits if nothing usable).
        static double AutoLevel(LevelSetting level) =>
            level.Key == GameSettings.HdrPeak.Key ? DisplayHdr.Main.Value?.AutoPeak ?? 1000
                                                  : DisplayHdr.Main.Value?.AutoPaperWhite ?? 200;

        static string Nits(double value) => value.ToString("#,0.##", CultureInfo.CurrentCulture) + " nits";

        // Peak brightness and paper white: Auto, or a slider in the pause
        // menu's steps. Dragging writes mcla.toml when the drag ends; each key
        // or controller step writes it at once.
        Element LevelSlider(LevelSetting level)
        {
            var file = level.Value(gameSettings);
            var auto = file <= 0;
            var autoValue = AutoLevel(level);
            var partnerLevel = GameSettings.HdrLevels.First(l => l.Key == level.Partner);
            var partnerFile = partnerLevel.Value(gameSettings);
            var partner = partnerFile > 0 ? partnerFile : AutoLevel(partnerLevel);
            var current = auto ? Math.Clamp(autoValue, level.Min, level.Max) : file;
            var shown = levelDrafts.TryGetValue(level.Key, out var draft) ? draft : current;
            // With Auto, what Windows reports, in whole nits (its peak has decimals).
            var text = !auto ? Nits(shown) : DisplayHdr.Main.Value is null ? "" : Nits(Math.Round(autoValue));
            void Step(int direction)
            {
                var next = level.StepFrom(file, direction);
                if (!(next >= level.Min)) next = level.Min;  // Auto is the checkbox here
                SetLevel(level, level.Limit(next, file, partner));
            }
            void EndDrag()
            {
                if (levelDrafts.TryGetValue(level.Key, out var value)) SetLevel(level, value);
            }
            var enabled = !gameRunning && !auto;
            return VStack(4,
                SettingTitle(level.Header, level.Info),
                Grid(
                    columns: [GridSize.Auto, GridSize.Star(), GridSize.Px(130)],
                    rows: [GridSize.Auto],
                    CheckBox(auto, v =>
                        {
                            if (v) SetLevel(level, 0);
                            else SetLevel(level, level.Limit(Math.Round(Math.Clamp(autoValue, level.Min, level.Max)), 0, partner));
                        }, "Auto")
                        .MinWidth(0)
                        .IsEnabled(!gameRunning)
                        .Grid(column: 0),
                    Slider(shown, level.Min, level.Max, v =>
                        {
                            if (!enabled || Math.Abs(v - shown) < 0.5) return;
                            setLevelDrafts(levelDrafts.SetItem(level.Key, level.Limit(level.Snap(v), file, partner)));
                        })
                        .StepFrequency(1)
                        .ThumbToolTip(false)
                        .IsEnabled(enabled)
                        .AutomationName(level.Header)
                        .OnPreviewKeyDown((_, e) =>
                        {
                            if (!enabled) return;
                            var direction = e.Key switch
                            {
                                VirtualKey.Left or VirtualKey.Down or VirtualKey.GamepadDPadLeft
                                    or VirtualKey.GamepadLeftThumbstickLeft => -1,
                                VirtualKey.Right or VirtualKey.Up or VirtualKey.GamepadDPadRight
                                    or VirtualKey.GamepadLeftThumbstickRight => 1,
                                _ => 0,
                            };
                            if (e.Key is VirtualKey.Home or VirtualKey.End)
                            {
                                e.Handled = true;
                                SetLevel(level, level.Limit(e.Key == VirtualKey.Home ? level.Min : level.Max, file, partner));
                                return;
                            }
                            if (direction == 0) return;
                            e.Handled = true;
                            Step(direction);
                        })
                        .OnPointerCaptureLost((_, _) => EndDrag())
                        .OnPointerReleased((_, _) => EndDrag())
                        .Margin(8, 0, 8, 0)
                        .VAlign(VerticalAlignment.Center)
                        .Grid(column: 1),
                    TextBlock(text)
                        .Foreground(auto ? Theme.SecondaryText : Theme.PrimaryText)
                        .VAlign(VerticalAlignment.Center)
                        .Grid(column: 2)));
        }

        static Element OptionCheckBox(bool value, Action<bool> onChanged, string label, string info) =>
            HStack(2, CheckBox(value, onChanged, label), InfoTip(label, info));

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
                Pairs(GameSettings.GraphicsFor(gameSettings, DisplayRefresh.Main.Value, GpuVendor.For(ShaderStamp.LastAdapterVendor(GameOptions.Arguments(settingsRef.Current), Paths.BuildDir)),
                                               DisplayDrivers.AutomaticApi(int.TryParse(ShaderStamp.Setting("d3d12_adapter", GameOptions.Arguments(settingsRef.Current), gameSettings), out var adapterOption) ? adapterOption : -1)).Select(SettingCombo).ToArray()),
                Subtitle("HDR"),
                Pairs([.. GameSettings.HdrChoices.Select(SettingCombo), .. GameSettings.HdrLevels.Select(LevelSlider)]),
                VStack(2, (DisplayHdr.Main.Value?.Describe() ?? DisplayHdr.Unknown)
                    .Select((line, i) => TextBlock(line).Foreground(Theme.SecondaryText).WithKey(i.ToString())).ToArray()),
                Subtitle("Game"),
                Pairs([.. GameSettings.Game.Select(SettingCombo), PlayerNameBox()]),
                // Lines short enough for the window: in this scroll viewer a
                // TextBlock is measured without a width limit, so it doesn't wrap.
                VStack(2, preparing
                    ? [TextBlock("Shaders are being prepared with these settings.").Foreground(Theme.SecondaryText),
                       TextBlock("They can be changed again in a few seconds, when it's done.").Foreground(Theme.SecondaryText)]
                    : gameRunning
                    ? [TextBlock("The game is running: change these in its pause menu, on the Settings tab.").Foreground(Theme.SecondaryText),
                       TextBlock("They show here again once it has closed.").Foreground(Theme.SecondaryText)]
                    : [TextBlock("These are the game's own settings, in mcla.toml next to mcla.exe. Its pause menu changes them too.").Foreground(Theme.SecondaryText),
                       TextBlock("Resolution, Graphics API, Language, Intro and Player name take effect when the game starts.").Foreground(Theme.SecondaryText)]),
                Expander("Advanced",
                    VStack(16,
                        VStack(4,
                            SettingTitle("More mcla.exe options", GameSettings.ExtraArgumentsInfo),
                            TextBox(settings.ExtraArguments, v => updateSettings(s => s with { ExtraArguments = v }),
                                    placeholderText: "for example --log_file=run.log")
                                .AutomationName("More mcla.exe options"),
                            TextBlock("Passed to mcla.exe at every start; they win over the settings above.")
                                .Foreground(Theme.SecondaryText)),
                        VStack(4,
                            OptionCheckBox(settings.CheckForUpdates, v => updateSettings(s => s with { CheckForUpdates = v }),
                                           "Check for updates when the launcher opens", GameSettings.CheckForUpdatesInfo),
                            OptionCheckBox(settings.CloseOnLaunch, v => updateSettings(s => s with { CloseOnLaunch = v }),
                                           "Close the launcher when the game starts", GameSettings.CloseOnLaunchInfo),
                            OptionCheckBox(settings.AutoPlay, v => updateSettings(s => s with { AutoPlay = v }),
                                           "Start the game right away when there's nothing to update", GameSettings.AutoPlayInfo)))
                ).HAlign(HorizontalAlignment.Stretch).HorizontalContentAlignment(HorizontalAlignment.Stretch)
            ).Padding(0, 0, 12, 0)).Margin(0, 16, 0, 0);

        Element Actions()
        {
            var buttons = new List<Element>();
            if (busy)
                buttons.Add(Button("Stop build", () => { updateCtsRef.Current?.Cancel(); buildRef.Current?.Stop(); }));
            else if (status != null && error == null)
            {
                if (status.UpdateAvailable && installed && status.Target is { IsMain: true } && status.PatchesChanged)
                    buttons.Add(Button("Follow the releases again", SwitchChannel).AccentButton());
                else if (status.UpdateAvailable)
                    buttons.Add(Button(status.Target?.SwitchBack == true ? "Go back to the release" : "Update and rebuild", () => StartBuild(BuildKind.Update)).AccentButton());
                else if (!status.GameBuilt)
                    buttons.Add(Button("Build the game", () => StartBuild(BuildKind.Build)).AccentButton());
                else if (!status.BuildCurrent || failed)
                    buttons.Add(Button("Rebuild", () => StartBuild(BuildKind.Build)).AccentButton());
            }

            if (preparing)
            {
                buttons.Add(Button("Stop", StopPrepare)
                    .AutomationName("Stop preparing shaders")
                    .ToolTip("End the shader preparation and don't start the game"));
                buttons.Add(Button(HStack(8, Icon(Symbol.Play), TextBlock("Play now")), PlayNow)
                    .AccentButton()
                    .AutomationName("Play now")
                    .ToolTip("End the shader preparation and start the game now. The game adds the shader pack by itself when it starts.")
                    .MinWidth(120));
                return HStack(8, buttons.ToArray()).HAlign(HorizontalAlignment.Right).Margin(0, 16, 0, 0);
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
            VStack(8, DevBar(), StatusBar(), WhatsNew(), LauncherUpdateBar(), CountdownBar(), NoticeBar()).Margin(0, 16, 0, 0).Grid(row: 1),
            MainPane().Grid(row: 2),
            ActivityRow().Grid(row: 3),
            Actions().Grid(row: 4),
            exitDialog
        ).Padding(24);
    }
}

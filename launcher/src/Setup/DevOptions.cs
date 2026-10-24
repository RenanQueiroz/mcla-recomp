// Options for developing and testing the setup wizard. None of them does anything unless it is given, a
// player's run never has them, and the wizard says so in its window when one is in use (Banner).
//
//   MCLA_RELEASE_SOURCE or --release-source=<folder or file:// URL>
//                   release.json and the assets from a folder, in place of GitHub's latest release;
//                   dev-source.json in it may name the git URL to clone (a local repository)
//   --install-dir=<folder>   the install folder, filled in (and the Welcome page's check still applies)
//   --game=<.iso or folder>  the game, filled in
//   --in-place               "use it where it is"
//   --auto                   go through the pages by itself once the release is known (with the two above)
//   MCLA_WIZARD_GAME_ARGS    options for mcla.exe (for the shader preparation, and settings.json of the new
//                   install): --user_data_root and --cache_root of a test run, so that it never touches
//                   the real saves or shader cache
//   MCLA_DEV_SHOTS=<folder>  a PNG of the window at each page, saved there
//   --quit-after=<seconds>   close the window after that long (for a run nobody is watching)
namespace MclaLauncher.Setup;

sealed class DevOptions
{
    public string? ReleaseSource { get; init; }
    public string? InstallDir { get; init; }
    public string? GamePath { get; init; }
    public bool InPlace { get; init; }
    public bool Auto { get; init; }
    public string GameArguments { get; init; } = "";
    public string? ShotsDir { get; init; }
    public int QuitAfterSeconds { get; init; }

    public bool Any => ReleaseSource != null || InstallDir != null || GamePath != null || InPlace || Auto || GameArguments.Length > 0 || ShotsDir != null || QuitAfterSeconds > 0;

    public static DevOptions Current { get; } = Parse(Environment.GetCommandLineArgs().Skip(1), Environment.GetEnvironmentVariable);

    public static DevOptions Parse(IEnumerable<string> args, Func<string, string?> environment)
    {
        string? Option(string name) => args.FirstOrDefault(a => a.StartsWith($"--{name}=", StringComparison.Ordinal))?[(name.Length + 3)..].Trim('"');
        string? Env(string name) => environment(name) is { Length: > 0 } value ? value : null;
        return new DevOptions
        {
            ReleaseSource = Option("release-source") ?? Env(Setup.ReleaseSource.EnvironmentVariable),
            InstallDir = Option("install-dir"),
            GamePath = Option("game"),
            InPlace = args.Contains("--in-place"),
            Auto = args.Contains("--auto"),
            GameArguments = Env("MCLA_WIZARD_GAME_ARGS") ?? "",
            ShotsDir = Env("MCLA_DEV_SHOTS"),
            QuitAfterSeconds = int.TryParse(Option("quit-after"), out var quit) ? quit : 0,
        };
    }

    /// What the window says when any of this is in use.
    public string Banner()
    {
        var parts = new List<string>();
        if (ReleaseSource != null) parts.Add($"release source: {ReleaseSource}");
        if (GameArguments.Length > 0) parts.Add($"game options: {GameArguments}");
        if (InstallDir != null) parts.Add("install folder given");
        if (GamePath != null) parts.Add("game given");
        if (Auto) parts.Add("automatic");
        if (ShotsDir != null) parts.Add($"screenshots to {ShotsDir}");
        if (QuitAfterSeconds > 0) parts.Add($"closes after {QuitAfterSeconds} s");
        return string.Join("; ", parts);
    }
}

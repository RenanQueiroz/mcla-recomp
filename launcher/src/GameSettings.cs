// The game's own settings, which the launcher edits in mcla.toml next to
// mcla.exe: the same file, and the same values, as the Settings tab of the
// game's pause menu (src/settings_menu.cpp).
using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace MclaLauncher;

/// One choice in a setting's drop-down and the value it stands for: a long,
/// a bool or a string, as the game's option takes it.
sealed record SettingChoice(string Label, object Value);

/// One of the game's settings: its option (a key in mcla.toml), its choices,
/// and the game's default, which applies while the file doesn't set it.
sealed record GameSetting(string Key, string Header, SettingChoice[] Choices, object Default)
{
    /// The choices to show for the file's value, and which one it is. A value
    /// that isn't one of the choices (set by hand) is shown as one more.
    public (SettingChoice[] Choices, int Index) Show(IReadOnlyDictionary<string, string> file)
    {
        if (!file.TryGetValue(Key, out var raw))
            return (Choices, Math.Max(0, Array.FindIndex(Choices, c => SettingValues.Same(c.Value, Default))));
        var value = SettingValues.Parse(raw);
        var index = Array.FindIndex(Choices, c => SettingValues.Same(c.Value, value));
        if (index >= 0) return (Choices, index);
        return ([.. Choices, new SettingChoice($"{SettingValues.Display(value)} (set in mcla.toml)", value)], Choices.Length);
    }
}

static class GameSettings
{
    // The choices are the pause menu's (src/settings_menu.cpp); 40 to 50 FPS
    // are for G-Sync/FreeSync displays, where a cap the game holds even in its
    // heaviest scenes gives constant frame times.
    public static readonly GameSetting Resolution = new("resolution_scale", "Resolution",
        [new("Native (720p)", 1L), new("2× (1440p)", 2L), new("3× (2160p, 4K)", 3L)], 1L);
    public static readonly GameSetting FrameRate = new("mcla_fps", "Frame rate",
        [new("30 FPS", 30L), new("40 FPS", 40L), new("45 FPS", 45L), new("50 FPS", 50L),
         new("60 FPS", 60L), new("120 FPS", 120L), new("Uncapped", 0L)], 30L);
    public static readonly GameSetting Display = new("fullscreen", "Display",
        [new("Fullscreen", true), new("Windowed", false)], true);
    public static readonly GameSetting AntiAliasing = new("mcla_msaa", "Anti-aliasing",
        [new("On (2× MSAA)", true), new("Off (about 20% faster at 3×)", false)], true);
    public static readonly GameSetting HudRate = new("mcla_ui_hud_rate", "HUD updates",
        [new("30 per second (as on the console)", 30L), new("60 per second", 60L),
         new("120 per second", 120L), new("Every frame", 0L)], 30L);
    public static readonly GameSetting GraphicsApi = new("mcla_gpu_backend", "Graphics API",
        [new("Automatic (Direct3D 12 first)", "any"), new("Direct3D 12", "d3d12"), new("Vulkan", "vulkan")], "any");
    public static readonly GameSetting Intro = new("mcla_intro", "Intro",
        [new("Normal", "normal"), new("Fast", "fast"), new("Skip (hides the title logo)", "skip")], "normal");
    public static readonly GameSetting Prompts = new("mcla_button_prompts", "Button prompts",
        [new("Follow the controller", "auto"), new("Xbox", "xbox"), new("PlayStation", "playstation")], "auto");

    public static readonly GameSetting[] Graphics = [Resolution, FrameRate, Display, AntiAliasing, HudRate, GraphicsApi];
    public static readonly GameSetting[] Game = [Intro, Prompts];
}

/// Values as mcla.toml holds them.
static class SettingValues
{
    public static object Parse(string raw)
    {
        raw = raw.Trim();
        if (raw == "true") return true;
        if (raw == "false") return false;
        if (long.TryParse(raw.Replace("_", ""), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var whole)) return whole;
        if (double.TryParse(raw.Replace("_", ""), NumberStyles.Float, CultureInfo.InvariantCulture, out var real)) return real;
        if (raw.Length >= 2 && raw[0] == '\'' && raw[^1] == '\'') return raw[1..^1];
        // A double-quoted value with a backslash that is no TOML escape (old
        // saves wrote Windows paths that way) is taken as written, whole, as
        // the game does.
        if (raw.Length >= 2 && raw[0] == '"' && raw[^1] == '"')
            return ValidEscapes(raw[1..^1]) ? Unescape(raw[1..^1]) : raw[1..^1];
        return raw;
    }

    public static bool ValidEscapes(string body)
    {
        for (var i = 0; i < body.Length; i++)
        {
            if (body[i] != '\\') continue;
            var next = i + 1 < body.Length ? body[i + 1] : '\0';
            if (next != '\0' && "btnfr\"\\".Contains(next)) { i++; continue; }
            if (next == 'u' && TryHex(body, i + 2, 4, out _)) { i += 5; continue; }
            if (next == 'U' && TryHex(body, i + 2, 8, out _)) { i += 9; continue; }
            return false;
        }
        return true;
    }

    public static bool Same(object a, object b) => (a, b) switch
    {
        (long x, long y) => x == y,
        (long x, double y) => x == y,
        (double x, long y) => x == y,
        (double x, double y) => x == y,
        (bool x, bool y) => x == y,
        (string x, string y) => x == y,
        _ => false,
    };

    public static string Display(object value) => value switch
    {
        double d => d.ToString(CultureInfo.InvariantCulture),
        long l => l.ToString(CultureInfo.InvariantCulture),
        bool b => b ? "true" : "false",
        _ => value.ToString() ?? "",
    };

    public static string Toml(object value) => value switch
    {
        bool b => b ? "true" : "false",
        long l => l.ToString(CultureInfo.InvariantCulture),
        double d => d.ToString("R", CultureInfo.InvariantCulture),
        string s => Quote(s),
        _ => throw new ArgumentException($"no TOML form for {value.GetType().Name}"),
    };

    /// A TOML basic string, escaped as the game writes one.
    public static string Quote(string text)
    {
        var sb = new StringBuilder("\"");
        foreach (var c in text)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\t': sb.Append("\\t"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                default:
                    if (c < 0x20 || c == 0x7F) sb.Append($"\\u{(int)c:X4}");
                    else sb.Append(c);
                    break;
            }
        }
        return sb.Append('"').ToString();
    }

    static string Unescape(string body)
    {
        var sb = new StringBuilder();
        for (var i = 0; i < body.Length; i++)
        {
            if (body[i] != '\\' || i + 1 >= body.Length) { sb.Append(body[i]); continue; }
            var next = body[++i];
            switch (next)
            {
                case 'b': sb.Append('\b'); break;
                case 't': sb.Append('\t'); break;
                case 'n': sb.Append('\n'); break;
                case 'f': sb.Append('\f'); break;
                case 'r': sb.Append('\r'); break;
                case '"': sb.Append('"'); break;
                case '\\': sb.Append('\\'); break;
                case 'u' when TryHex(body, i + 1, 4, out var u):
                    sb.Append(char.ConvertFromUtf32(u)); i += 4; break;
                case 'U' when TryHex(body, i + 1, 8, out var U):
                    sb.Append(char.ConvertFromUtf32(U)); i += 8; break;
                default: sb.Append('\\').Append(next); break; // as old saves meant it
            }
        }
        return sb.ToString();
    }

    static bool TryHex(string text, int at, int count, out int value)
    {
        value = 0;
        return at + count <= text.Length
            && int.TryParse(text.AsSpan(at, count), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out value);
    }
}

/// mcla.toml, edited the way the game's pause menu edits it: a key's line is
/// replaced where it is or added before the first [table], every other line
/// (comments, keys the launcher doesn't know, tables) is kept, and the file is
/// written beside the old one and renamed over it.
static class GameSettingsFile
{
    public static string PathIn(string buildDir) => Path.Combine(buildDir, "mcla.toml");

    static readonly Regex KeyLine = new(@"^\s*([A-Za-z0-9_-]+)\s*=(.*)$");

    static bool IsTableHeader(string line) => line.TrimStart().StartsWith('[');

    /// The file's top-level keys and their values as written (an empty map
    /// when there is no file).
    public static Dictionary<string, string> Read(string path)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        if (!File.Exists(path)) return values;
        foreach (var line in File.ReadAllLines(path))
        {
            if (IsTableHeader(line)) break;
            var match = KeyLine.Match(line);
            if (match.Success) values[match.Groups[1].Value] = ValueText(match.Groups[2].Value);
        }
        return values;
    }

    /// The value part of a `key = value` line without a trailing comment.
    static string ValueText(string rest)
    {
        rest = rest.Trim();
        if (rest.Length == 0) return rest;
        if (rest[0] is '"' or '\'')
        {
            var quote = rest[0];
            for (var i = 1; i < rest.Length; i++)
            {
                if (quote == '"' && rest[i] == '\\') { i++; continue; }
                if (rest[i] == quote) return rest[..(i + 1)];
            }
            return rest;
        }
        var hash = rest.IndexOf('#');
        return (hash >= 0 ? rest[..hash] : rest).Trim();
    }

    /// Sets these keys, merging them into the file (created when missing).
    public static void Set(string path, IReadOnlyDictionary<string, object> values)
    {
        var lines = new List<string>();
        var crlf = false;
        if (File.Exists(path))
        {
            var text = File.ReadAllText(path);
            crlf = text.Contains("\r\n");
            lines.AddRange(text.Replace("\r\n", "\n").Split('\n'));
            if (lines.Count > 0 && lines[^1].Length == 0) lines.RemoveAt(lines.Count - 1);
        }
        if (lines.Count == 0) lines.Add("# Settings of the game, kept by its pause menu and the launcher");

        var topEnd = lines.FindIndex(IsTableHeader);
        if (topEnd < 0) topEnd = lines.Count;
        var written = new HashSet<string>(StringComparer.Ordinal);
        for (var i = 0; i < topEnd; i++)
        {
            var match = KeyLine.Match(lines[i]);
            if (match.Success && values.TryGetValue(match.Groups[1].Value, out var value))
            {
                lines[i] = $"{match.Groups[1].Value} = {SettingValues.Toml(value)}";
                written.Add(match.Groups[1].Value);
            }
            else if (Mended(lines[i]) is { } mended)
            {
                lines[i] = mended;
            }
        }
        var added = values.Where(kv => !written.Contains(kv.Key))
                          .Select(kv => $"{kv.Key} = {SettingValues.Toml(kv.Value)}");
        lines.InsertRange(topEnd, added);

        var temporary = path + ".tmp";
        var newline = crlf ? "\r\n" : "\n";
        File.WriteAllText(temporary, string.Join(newline, lines) + newline, new UTF8Encoding(false));
        File.Move(temporary, path, overwrite: true);
    }

    /// A double-quoted value whose backslashes aren't TOML escapes, as old
    /// saves of the game wrote Windows paths, as the same text in a literal
    /// string; null when the line needs nothing (as the game's MendedLine).
    static string? Mended(string line)
    {
        var match = KeyLine.Match(line);
        if (!match.Success) return null;
        var equals = line.IndexOf('=');
        var open = line.IndexOf('"', equals + 1);
        var close = line.LastIndexOf('"');
        if (open < 0 || close <= open || line[(equals + 1)..open].Trim().Length > 0) return null;
        var body = line[(open + 1)..close];
        if (body.Contains('\'') || body.Contains('"')) return null;
        for (var i = 0; i < body.Length; i++)
        {
            if (body[i] != '\\') continue;
            var next = i + 1 < body.Length ? body[i + 1] : '\0';
            if ("btnfr\\".Contains(next) && next != '\0') { i++; continue; }
            if (next == 'u' && IsHex(body, i + 2, 4)) { i += 5; continue; }
            if (next == 'U' && IsHex(body, i + 2, 8)) { i += 9; continue; }
            return line[..open] + "'" + body + "'" + line[(close + 1)..];
        }
        return null;
    }

    static bool IsHex(string text, int at, int count) =>
        at + count <= text.Length && text.AsSpan(at, count).ToString().All(Uri.IsHexDigit);
}

/// Before 2026-10-16 the launcher passed its choices on the command line every
/// start, which beat what the pause menu saved. Now it edits mcla.toml, so on
/// the first start of this launcher the choices that were forcing a value are
/// written there once, after a copy of the file is kept.
static class SettingsMigration
{
    public const string BackupName = "mcla.toml.before-launcher-settings";

    // What each old drop-down passed, by its saved index; null for "Game setting".
    static readonly object?[] OldDisplay = [null, true, false];
    static readonly object?[] OldResolution = [1L, 2L, 3L];
    static readonly object?[] OldFps = [null, 30L, 40L, 45L, 50L, 60L, 120L, 0L];
    static readonly object?[] OldIntro = [null, "normal", "fast", "skip"];
    static readonly object?[] OldPrompts = [null, "auto", "xbox", "playstation"];
    static readonly object?[] OldAntiAliasing = [null, true, false];
    static readonly object?[] OldGraphicsApi = [null, "d3d12", "vulkan"];
    static readonly object?[] OldHudRate = [30L, 60L, 120L, 0L];

    /// The values the old launcher forced, from a settings.json it saved.
    public static Dictionary<string, object> ForcedValues(LauncherSettings s)
    {
        var values = new Dictionary<string, object>(StringComparer.Ordinal);
        void Add(GameSetting setting, object?[] old, int index)
        {
            if (index >= 0 && index < old.Length && old[index] is { } value) values[setting.Key] = value;
        }
        Add(GameSettings.Display, OldDisplay, s.Display);
        Add(GameSettings.Resolution, OldResolution, s.Resolution);
        Add(GameSettings.FrameRate, OldFps, s.Fps);
        Add(GameSettings.Intro, OldIntro, s.Intro);
        Add(GameSettings.Prompts, OldPrompts, s.Prompts);
        Add(GameSettings.AntiAliasing, OldAntiAliasing, s.AntiAliasing);
        Add(GameSettings.GraphicsApi, OldGraphicsApi, s.GraphicsApi);
        Add(GameSettings.HudRate, OldHudRate, s.HudRate);
        return values;
    }

    /// Runs once: `existed` is whether a settings.json was there to read (a
    /// new install has nothing to carry over). The copy of mcla.toml is made
    /// before anything is written, and never replaced; running again writes
    /// the same values. On a failure the flag stays clear, to try next time.
    public static LauncherSettings Run(LauncherSettings s, bool existed, string buildDir)
    {
        if (s.SettingsInGameFile) return s;
        if (existed)
        {
            try
            {
                var values = ForcedValues(s);
                if (values.Count > 0)
                {
                    Directory.CreateDirectory(buildDir);
                    var path = GameSettingsFile.PathIn(buildDir);
                    var backup = Path.Combine(buildDir, BackupName);
                    if (File.Exists(path) && !File.Exists(backup)) File.Copy(path, backup);
                    GameSettingsFile.Set(path, values);
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                Debug.WriteLine($"settings migration: {ex.Message}");
                return s;
            }
        }
        return s with { SettingsInGameFile = true };
    }
}

// The game's own settings, which the launcher edits in mcla.toml next to
// mcla.exe: the same file, and the same values, as the Settings tab of the
// game's pause menu (src/settings_menu.cpp, which saves through the SDK's
// rex::cvar::SaveConfigValues; this file follows the same rules).
using System.Diagnostics;
using System.Globalization;
using System.Text;

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
    // The console's gamma ramp for an HDTV, or sRGB, which a PC monitor
    // expects (src/display_gamma.cpp); it changes during play too.
    public static readonly GameSetting DisplayGamma = new("mcla_display_gamma", "Display gamma",
        [new("TV (as on the console)", "tv"), new("PC monitor (sRGB, brighter shadows)", "pc")], "tv");
    public static readonly GameSetting AntiAliasing = new("mcla_msaa", "Anti-aliasing",
        [new("On (2× MSAA)", true), new("Off (2 to 5% faster at 3×)", false)], true);
    // With anti-aliasing on, the scene drawn once instead of in the Xbox 360's
    // two tiles, in the larger EDRAM the game sets up on PC
    // (src/render_perf.cpp); it changes during play too.
    public static readonly GameSetting SingleTile = new("mcla_single_tile", "Single tile rendering",
        [new("On (9 to 20% faster at 3×)", true), new("Off (two tiles, as on the console)", false)], true);
    public static readonly GameSetting HudRate = new("mcla_ui_hud_rate", "HUD updates",
        [new("30 per second (as on the console)", 30L), new("60 per second", 60L),
         new("120 per second", 120L), new("Every frame", 0L)], 30L);
    public static readonly GameSetting GraphicsApi = new("mcla_gpu_backend", "Graphics API",
        [new("Automatic (Direct3D 12 first)", "any"), new("Direct3D 12", "d3d12"), new("Vulkan", "vulkan")], "any");
    public static readonly GameSetting Intro = new("mcla_intro", "Intro",
        [new("Normal", "normal"), new("Fast", "fast"), new("Skip (hides the title logo)", "skip")], "normal");
    public static readonly GameSetting Prompts = new("mcla_button_prompts", "Button prompts",
        [new("Follow the controller", "auto"), new("Xbox", "xbox"), new("PlayStation", "playstation")], "auto");

    public static readonly GameSetting[] Graphics = [Resolution, FrameRate, Display, DisplayGamma, AntiAliasing, SingleTile, HudRate, GraphicsApi];
    public static readonly GameSetting[] Game = [Intro, Prompts];

    /// Started by the launcher, from its own shortcut, or again by its pause
    /// menu's RESTART GAME: any mcla.exe counts, as for the build.
    public static bool GameIsRunning()
    {
        var processes = Process.GetProcessesByName("mcla");
        foreach (var process in processes) process.Dispose();
        return processes.Length > 0;
    }
}

/// Values as mcla.toml holds them. Parsing never throws: a value it can't make
/// sense of comes back as its text, which the drop-down shows as set by hand.
static class SettingValues
{
    public static object Parse(string raw)
    {
        try
        {
            raw = raw.Trim();
            if (raw == "true") return true;
            if (raw == "false") return false;
            if (long.TryParse(raw.Replace("_", ""), NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var whole)) return whole;
            if (double.TryParse(raw.Replace("_", ""), NumberStyles.Float, CultureInfo.InvariantCulture, out var real)) return real;
            if (raw.Length >= 2 && raw[0] == '\'' && raw[^1] == '\'') return raw[1..^1];
            // A double-quoted value with a backslash that is no TOML escape
            // (older saves wrote Windows paths that way) is taken as written,
            // whole, as the game does.
            if (raw.Length >= 2 && raw[0] == '"' && raw[^1] == '"')
                return ValidEscapes(raw[1..^1]) ? Unescape(raw[1..^1]) : raw[1..^1];
            return raw;
        }
        catch (Exception ex) when (ex is ArgumentException or FormatException or OverflowException)
        {
            return raw;
        }
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

    /// Whether a basic string's body has only TOML escapes, \u and \U naming
    /// a Unicode scalar value.
    public static bool ValidEscapes(string body)
    {
        for (var i = 0; i < body.Length; i++)
        {
            if (body[i] != '\\') continue;
            var next = i + 1 < body.Length ? body[i + 1] : '\0';
            if (next != '\0' && "btnfr\"\\".Contains(next)) { i++; continue; }
            var digits = next == 'u' ? 4 : next == 'U' ? 8 : 0;
            if (digits == 0 || !TryHex(body, i + 2, digits, out var code) || !IsScalar(code)) return false;
            i += 1 + digits;
        }
        return true;
    }

    static bool IsScalar(long code) => code <= 0x10FFFF && (code < 0xD800 || code > 0xDFFF);

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
                case 'u' when TryHex(body, i + 1, 4, out var u) && IsScalar(u):
                    sb.Append(char.ConvertFromUtf32((int)u)); i += 4; break;
                case 'U' when TryHex(body, i + 1, 8, out var U) && IsScalar(U):
                    sb.Append(char.ConvertFromUtf32((int)U)); i += 8; break;
                default: sb.Append('\\').Append(next); break;
            }
        }
        return sb.ToString();
    }

    static bool TryHex(string text, int at, int count, out long value)
    {
        value = 0;
        return at + count <= text.Length
            && long.TryParse(text.AsSpan(at, count), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out value);
    }
}

/// mcla.toml, edited the way the game edits it (rex::cvar::SaveConfigValues
/// in the SDK patch rexglue-cvar-toml-strings.patch): a key's `key = value`
/// statement is replaced where it is, keeping a comment after it, or added
/// before the first [table]; every other line (comments, keys the launcher
/// doesn't know, tables, values over several lines) is kept; the file is
/// written beside the old one, flushed to the disk and renamed over it.
static class GameSettingsFile
{
    public static string PathIn(string buildDir) => Path.Combine(buildDir, "mcla.toml");

    sealed class Text
    {
        public List<string> Lines = [];  // without line endings or a BOM
        public bool Bom;
        public bool Crlf;
    }

    sealed record Statement(string Key, string Table, int First, int Last, int ValueCol, int ValueEnd);

    static Text Split(string text)
    {
        var result = new Text();
        if (text.StartsWith('﻿')) { result.Bom = true; text = text[1..]; }
        var lines = text.Split('\n');
        var count = lines.Length > 0 && lines[^1].Length == 0 ? lines.Length - 1 : lines.Length;
        for (var i = 0; i < count; i++)
        {
            var line = lines[i];
            if (line.EndsWith('\r')) { line = line[..^1]; result.Crlf = true; }
            result.Lines.Add(line);
        }
        return result;
    }

    static string Join(Text text)
    {
        var sb = new StringBuilder(text.Bom ? "﻿" : "");
        foreach (var line in text.Lines) sb.Append(line).Append(text.Crlf ? "\r\n" : "\n");
        return sb.ToString();
    }

    /// Past the value starting at (li, ci): across lines for a multi-line
    /// string or an array, up to the whitespace or comment after it.
    static (int Line, int Col) SkipValue(List<string> lines, int li, int ci)
    {
        var depth = 0;
        (int, int) End() => (lines.Count - 1, lines[^1].Length);
        while (li < lines.Count)
        {
            var line = lines[li];
            if (ci >= line.Length)
            {
                if (depth == 0) return (li, line.Length);
                li++; ci = 0; continue;
            }
            var c = line[ci];
            if (string.CompareOrdinal(line, ci, "\"\"\"", 0, 3) == 0 || string.CompareOrdinal(line, ci, "'''", 0, 3) == 0)
            {
                var close = line.Substring(ci, 3);
                ci += 3;
                while (true)
                {
                    if (li >= lines.Count) return End();
                    if (ci >= lines[li].Length) { li++; ci = 0; continue; }
                    if (close[0] == '"' && lines[li][ci] == '\\') { ci += 2; continue; }
                    if (string.CompareOrdinal(lines[li], ci, close, 0, 3) == 0)
                    {
                        ci += 3;
                        for (var extra = 0; extra < 2 && ci < lines[li].Length && lines[li][ci] == close[0]; extra++) ci++;
                        break;
                    }
                    ci++;
                }
                if (depth == 0) return (li, ci);
            }
            else if (c is '"' or '\'')
            {
                ci++;
                while (ci < line.Length && line[ci] != c) ci += c == '"' && line[ci] == '\\' ? 2 : 1;
                ci = Math.Min(ci + 1, line.Length);
                if (depth == 0) return (li, ci);
            }
            else if (c is '[' or '{') { depth++; ci++; }
            else if (c is ']' or '}')
            {
                depth = Math.Max(0, depth - 1);
                ci++;
                if (depth == 0) return (li, ci);
            }
            else if (c == '#')
            {
                if (depth == 0) return (li, ci);
                ci = line.Length;
            }
            else if (c is ' ' or '\t' && depth == 0) return (li, ci);
            else ci++;
        }
        return End();
    }

    /// A key at line[pos], bare or quoted parts joined by dots, up to its '='.
    static bool ParseKey(string line, ref int pos, out string key)
    {
        var sb = new StringBuilder();
        key = "";
        while (true)
        {
            while (pos < line.Length && line[pos] is ' ' or '\t') pos++;
            if (pos >= line.Length) return false;
            if (line[pos] is '"' or '\'')
            {
                var end = line.IndexOf(line[pos], pos + 1);
                if (end < 0) return false;
                sb.Append(line, pos + 1, end - pos - 1);
                pos = end + 1;
            }
            else
            {
                var end = pos;
                while (end < line.Length && (char.IsAsciiLetterOrDigit(line[end]) || line[end] is '_' or '-')) end++;
                if (end == pos) return false;
                sb.Append(line, pos, end - pos);
                pos = end;
            }
            while (pos < line.Length && line[pos] is ' ' or '\t') pos++;
            if (pos < line.Length && line[pos] == '.') { sb.Append('.'); pos++; continue; }
            key = sb.ToString();
            return pos < line.Length && line[pos] == '=';
        }
    }

    static List<Statement> Scan(List<string> lines, out int firstTable)
    {
        var statements = new List<Statement>();
        var table = "";
        firstTable = lines.Count;
        for (var i = 0; i < lines.Count;)
        {
            var line = lines[i];
            var p = 0;
            while (p < line.Length && line[p] is ' ' or '\t') p++;
            if (p >= line.Length || line[p] == '#') { i++; continue; }
            if (line[p] == '[')
            {
                if (firstTable == lines.Count) firstTable = i;
                var close = line.IndexOf(']', p);
                table = line.AsSpan(p).StartsWith("[[") || close < 0 ? "\u0001" : line[(p + 1)..close].Trim();
                i++;
                continue;
            }
            var pos = p;
            if (!ParseKey(line, ref pos, out var key)) { i++; continue; }
            var valueCol = pos + 1;
            while (valueCol < line.Length && line[valueCol] is ' ' or '\t') valueCol++;
            var (last, valueEnd) = SkipValue(lines, i, valueCol);
            statements.Add(new Statement(key, table, i, last, valueCol, valueEnd));
            i = last + 1;
        }
        return statements;
    }

    /// The file's top-level keys and their values as written (an empty map
    /// when there is no file).
    public static Dictionary<string, string> Read(string path)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        if (!File.Exists(path)) return values;
        var text = Split(File.ReadAllText(path));
        foreach (var s in Scan(text.Lines, out _))
        {
            if (s.Table.Length > 0) continue;
            values[s.Key] = s.First == s.Last
                ? text.Lines[s.First][s.ValueCol..Math.Min(s.ValueEnd, text.Lines[s.First].Length)]
                : string.Join("\n", text.Lines.Skip(s.First).Take(s.Last - s.First + 1))[s.ValueCol..];
        }
        return values;
    }

    /// Sets these keys, merging them into the file (created when missing).
    /// Throws, leaving the file as it was, when it can't be read (or isn't
    /// UTF-8 text, which rewriting would damage) or written.
    public static void Set(string path, IReadOnlyDictionary<string, object> values)
    {
        var old = File.Exists(path) ? File.ReadAllBytes(path) : [];
        string content;
        try
        {
            content = new UTF8Encoding(false, throwOnInvalidBytes: true).GetString(old);
        }
        catch (DecoderFallbackException)
        {
            throw new IOException($"{path} isn't UTF-8 text; it was left as it is");
        }
        var text = Split(content);
        if (text.Lines.Count == 0) text.Lines.Add("# Settings of the game, kept by its pause menu and the launcher");

        var written = new HashSet<string>(StringComparer.Ordinal);
        var statements = Scan(text.Lines, out _);
        for (var k = statements.Count - 1; k >= 0; k--)  // from the end, so line numbers above hold
        {
            var s = statements[k];
            if (s.Table.Length == 0 && values.TryGetValue(s.Key, out var value))
            {
                var last = text.Lines[s.Last];
                var after = last[Math.Min(s.ValueEnd, last.Length)..];
                text.Lines[s.First] = text.Lines[s.First][..s.ValueCol] + SettingValues.Toml(value) + after;
                text.Lines.RemoveRange(s.First + 1, s.Last - s.First);
                written.Add(s.Key);
            }
            else if (s.First == s.Last && LiteralForBrokenString(text.Lines[s.First], s.ValueCol) is { } literal)
            {
                text.Lines[s.First] = literal;
            }
        }
        Scan(text.Lines, out var firstTable);
        text.Lines.InsertRange(firstTable, values.Where(kv => !written.Contains(kv.Key))
                                                 .Select(kv => $"{kv.Key} = {SettingValues.Toml(kv.Value)}"));
        WriteDurably(path, new UTF8Encoding(false).GetBytes(Join(text)));
    }

    /// A one-line double-quoted value whose backslashes aren't TOML escapes,
    /// as older saves of the game wrote Windows paths, as the same text in a
    /// literal string; null when it isn't one (as the SDK's).
    static string? LiteralForBrokenString(string line, int valueCol)
    {
        if (valueCol >= line.Length || line[valueCol] != '"' || string.CompareOrdinal(line, valueCol, "\"\"\"", 0, 3) == 0)
            return null;
        var close = line.LastIndexOf('"');
        if (close <= valueCol) return null;
        var body = line[(valueCol + 1)..close];
        var tail = line[(close + 1)..].Trim();
        if (body.IndexOfAny(['"', '\'']) >= 0 || (tail.Length > 0 && tail[0] != '#') || SettingValues.ValidEscapes(body))
            return null;
        return line[..valueCol] + "'" + body + "'" + line[(close + 1)..];
    }

    static void WriteDurably(string path, byte[] bytes)
    {
        var temporary = $"{path}.launcher-{Environment.ProcessId}.tmp";
        try
        {
            using (var stream = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None))
            {
                stream.Write(bytes);
                stream.Flush(flushToDisk: true);
            }
            File.Move(temporary, path, overwrite: true);
        }
        catch
        {
            try { File.Delete(temporary); } catch (IOException) { }
            throw;
        }
    }
}

/// Before 2026-10-16 the launcher passed its choices on the command line every
/// start, which beat what the pause menu saved. Now it edits mcla.toml, so the
/// first time this launcher opens, the choices that were forcing a value are
/// written there once, after a copy of the file is kept. That copy also marks
/// the move as done, besides SettingsInGameFile in settings.json, and the old
/// choices are dropped from settings.json once moved: a lost flag never brings
/// stale choices back over what the pause menu set since.
static class SettingsMigration
{
    public const string BackupName = "mcla.toml.before-launcher-settings";

    // What each old drop-down passed, by its saved index; null for "Game setting".
    static readonly object?[] OldDisplay = [null, true, false];
    static readonly object?[] OldResolution = [1L, 2L, 3L];
    static readonly object?[] OldFps = [null, 30L, 40L, 45L, 50L, 60L, 120L, 0L];
    static readonly object?[] OldFpsList0 = [null, 30L, 60L, 120L, 0L];  // before 40, 45 and 50
    static readonly object?[] OldIntro = [null, "normal", "fast", "skip"];
    static readonly object?[] OldPrompts = [null, "auto", "xbox", "playstation"];
    static readonly object?[] OldAntiAliasing = [null, true, false];
    static readonly object?[] OldGraphicsApi = [null, "d3d12", "vulkan"];
    static readonly object?[] OldHudRate = [30L, 60L, 120L, 0L];

    static bool HasOldChoices(LauncherSettings s) =>
        s.Display is not null || s.Resolution is not null || s.Fps is not null || s.Intro is not null ||
        s.Prompts is not null || s.AntiAliasing is not null || s.GraphicsApi is not null || s.HudRate is not null;

    /// The values the old launcher forced, from a settings.json it saved
    /// (an absent index meaning its default, as it did).
    public static Dictionary<string, object> ForcedValues(LauncherSettings s)
    {
        var values = new Dictionary<string, object>(StringComparer.Ordinal);
        if (!HasOldChoices(s)) return values;
        void Add(GameSetting setting, object?[] old, int index)
        {
            if (index >= 0 && index < old.Length && old[index] is { } value) values[setting.Key] = value;
        }
        Add(GameSettings.Display, OldDisplay, s.Display ?? 0);
        Add(GameSettings.Resolution, OldResolution, s.Resolution ?? 0);
        Add(GameSettings.FrameRate, (s.FpsList ?? 0) >= 1 ? OldFps : OldFpsList0, s.Fps ?? 0);
        // Every frame rate came with --vsync=false.
        if (values.ContainsKey(GameSettings.FrameRate.Key)) values["vsync"] = false;
        Add(GameSettings.Intro, OldIntro, s.Intro ?? 0);
        Add(GameSettings.Prompts, OldPrompts, s.Prompts ?? 0);
        Add(GameSettings.AntiAliasing, OldAntiAliasing, s.AntiAliasing ?? 0);
        Add(GameSettings.GraphicsApi, OldGraphicsApi, s.GraphicsApi ?? 0);
        Add(GameSettings.HudRate, OldHudRate, s.HudRate ?? 0);
        return values;
    }

    /// Marked done, with the old choices dropped.
    public static LauncherSettings Retire(LauncherSettings s) => s with
    {
        SettingsInGameFile = true,
        Display = null, Resolution = null, Fps = null, FpsList = null, Intro = null,
        Prompts = null, AntiAliasing = null, GraphicsApi = null, HudRate = null,
    };

    /// Moves the old choices into mcla.toml once. Not while the game runs
    /// (it writes the file itself; the launcher tries again when it closes),
    /// and not again once the copy of the file exists. The copy is made
    /// before anything is written (empty when there was no file) and taken
    /// back if the write fails, to try again next time.
    public static LauncherSettings Run(LauncherSettings s, string buildDir, Func<bool> gameRunning)
    {
        if (s.SettingsInGameFile) return s;
        var values = ForcedValues(s);
        var backup = Path.Combine(buildDir, BackupName);
        if (values.Count == 0 || File.Exists(backup)) return Retire(s);
        if (gameRunning()) return s;
        var path = GameSettingsFile.PathIn(buildDir);
        try
        {
            Directory.CreateDirectory(buildDir);
            if (File.Exists(path)) File.Copy(path, backup);
            else File.WriteAllBytes(backup, []);
            try
            {
                GameSettingsFile.Set(path, values);
            }
            catch
            {
                File.Delete(backup);
                throw;
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Debug.WriteLine($"settings migration: {ex.Message}");
            return s;
        }
        return Retire(s);
    }
}

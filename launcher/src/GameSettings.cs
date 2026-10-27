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
/// and the game's default, which applies while the file doesn't set it. Info
/// is what the "i" next to its title says (Tips).
sealed record GameSetting(string Key, string Header, SettingChoice[] Choices, object Default)
{
    public string Info { get; init; } = "";
    /// What the game takes while the file doesn't set Key, when that depends
    /// on another of its lines (an older option); null for Default.
    public Func<IReadOnlyDictionary<string, string>, object?>? DefaultFrom { get; init; }
    /// Whether a choice can be offered with the file's other settings (null:
    /// all can). One that can't but is the file's value is shown with
    /// Unavailable after it.
    public Func<IReadOnlyDictionary<string, string>, object, bool>? Offered { get; init; }
    public string Unavailable { get; init; } = "";

    /// The choices to show for the file's value, and which one it is. A value
    /// that isn't one of the choices (set by hand) is shown as one more.
    public (SettingChoice[] Choices, int Index) Show(IReadOnlyDictionary<string, string> file)
    {
        var choices = Offered is null ? Choices : Choices.Where(c => Offered(file, c.Value)).ToArray();
        if (!file.TryGetValue(Key, out var raw))
        {
            var fallback = DefaultFrom?.Invoke(file) ?? Default;
            return (choices, Math.Max(0, Array.FindIndex(choices, c => SettingValues.Same(c.Value, fallback))));
        }
        var value = SettingValues.Parse(raw);
        var index = Array.FindIndex(choices, c => SettingValues.Same(c.Value, value));
        if (index >= 0) return (choices, index);
        var hidden = Array.Find(Choices, c => SettingValues.Same(c.Value, value));
        var label = hidden is not null ? $"{hidden.Label} ({Unavailable})"
                                       : $"{SettingValues.Display(value)} (set in mcla.toml)";
        return ([.. choices, new SettingChoice(label, value)], choices.Length);
    }
}

/// A level in nits with a slider (the HDR peak and paper white): 0 (or no
/// line in mcla.toml) is Auto, what the display reports; otherwise Min to Max
/// in steps of Step, as the pause menu steps it (src/settings_menu.cpp). A
/// value set by hand between the steps is kept and shown as it is. It stays
/// at or above (PartnerBelow) or at or below the option Partner, Auto there
/// counting as what Windows reports: a value that would cross it is held back
/// at the last step that doesn't.
sealed record LevelSetting(string Key, string Header, double Min, double Max, double Step)
{
    public string Info { get; init; } = "";
    public string Partner { get; init; } = "";
    public bool PartnerBelow { get; init; }

    /// The file's value: 0 for Auto (no line, 0, or a value that isn't a number).
    public double Value(IReadOnlyDictionary<string, string> file)
    {
        if (!file.TryGetValue(Key, out var raw)) return 0;
        var value = SettingValues.Parse(raw) switch
        {
            long l => (double)l,
            double d => d,
            _ => 0.0,
        };
        return value > 0 && double.IsFinite(value) ? value : 0;
    }

    /// The nearest step to a value, within Min and Max.
    public double Snap(double value) =>
        Math.Clamp(Min + Math.Round((value - Min) / Step) * Step, Min, Max);

    /// The next value in a direction, as the pause menu steps: from Auto to
    /// Min, then by Step (a value between steps goes to the nearest step that
    /// way); the ends stay.
    public double StepFrom(double value, int direction)
    {
        const double slack = 1e-6;
        var position = (value - Min) / Step;
        if (direction > 0)
        {
            if (!(value > 0) || value < Min) return Min;
            var next = Min + (Math.Floor(position + slack) + 1) * Step;
            return next <= Max + slack ? next : value;
        }
        if (!(value > Min + slack)) return 0;
        if (value > Max) return Max;
        return Min + (Math.Ceiling(position - slack) - 1) * Step;
    }

    /// A value kept on its side of the partner's (its effective value given):
    /// held back at the last step that doesn't cross it, or `current` if that
    /// would move the other way.
    public double Limit(double value, double current, double partner)
    {
        if (!(value > 0) || !(partner > 0)) return value;
        const double slack = 1e-6;
        var position = (partner - Min) / Step;
        if (PartnerBelow && value < partner)
        {
            var held = Math.Clamp(Min + Math.Ceiling(position - slack) * Step, Min, Max);
            return value < current && held >= current ? current : held;
        }
        if (!PartnerBelow && value > partner)
        {
            var held = Math.Clamp(Min + Math.Floor(position + slack) * Step, Min, Max);
            return value > current && held <= current ? current : held;
        }
        return value;
    }
}

static class GameSettings
{
    // The choices are the pause menu's (src/settings_menu.cpp); 40 to 50 FPS
    // are for G-Sync/FreeSync displays, where a cap the game holds even in its
    // heaviest scenes gives constant frame times. The tips (Info) are checked
    // against the code and the measurements in README.md, KNOWN-ISSUES.md and
    // README.md here.
    public static readonly GameSetting Resolution = new("resolution_scale", "Resolution",
        [new("Native (720p)", 1L), new("2× (1440p)", 2L), new("3× (2160p, 4K)", 3L)], 1L)
    {
        Info = "How sharp the 3D picture is: the game's own 720p, or 2× or 3× that. Pick the one closest to "
             + "your monitor (2× for 1080p and 1440p monitors, 3× for 4K), and go lower if your graphics card "
             + "can't keep up; 3× needs by far the most GPU power. Takes effect the next time the game starts.",
    };
    public static readonly GameSetting FrameRate = new("mcla_fps", "Frame rate",
        [new("30 FPS", 30L), new("40 FPS", 40L), new("45 FPS", 45L), new("50 FPS", 50L),
         new("60 FPS", 60L), new("120 FPS", 120L), new("Uncapped", 0L)], 30L)
    {
        Info = "The frame-rate cap. The game was made for 30 FPS, the console's rate. 40, 45 and 50 suit "
             + "G-Sync or FreeSync displays, where a cap the game always holds gives even frame times. Use 60 "
             + "for a 60 Hz display, and 120 or Uncapped for high-refresh displays if your PC keeps up.",
    };
    public static readonly GameSetting Display = new("fullscreen", "Display",
        [new("Fullscreen", true), new("Windowed", false)], true)
    {
        Info = "Fullscreen covers the whole screen; Windowed runs the game in a window on the desktop.",
    };
    // The console's gamma ramp for an HDTV, or sRGB, which a PC monitor
    // expects (src/display_gamma.cpp); it changes during play too.
    public static readonly GameSetting DisplayGamma = new("mcla_display_gamma", "Display gamma",
        [new("TV (as on the console)", "tv"), new("PC monitor (sRGB, brighter shadows)", "pc")], "tv")
    {
        Info = "How dark the shadows look. TV (the default) keeps the console's look, with deeper, moodier "
             + "shadows; PC monitor shows the shadows as the game drew them, with more detail in dark areas. "
             + "Either is fine on any screen; pick the look you prefer. It applies in HDR too, and switches "
             + "during play in the pause menu.",
    };
    // mcla_anti_aliasing (src/perf_options.cpp): the game's 2× MSAA, AMD FSR
    // 3.1 at native resolution (the SDK's temporal_aa,
    // rexglue-temporal-aa.patch), or none; all three change during play too.
    // Before it, mcla_msaa = false meant none, and still does while
    // mcla_anti_aliasing isn't in the file.
    public static readonly GameSetting AntiAliasing = new("mcla_anti_aliasing", "Anti-aliasing",
        [new("MSAA 2× (as on the console)", "msaa"), new("FSR 3.1 native AA (steadiest, softer)", "fsr"),
         new("Off (2 to 5% faster at 3× than MSAA)", "off")], "msaa")
    {
        Info = "Smooths jagged edges. MSAA 2× (the default) is the console's own: it smooths the outlines of "
             + "cars and buildings, but not the shimmer on fine detail such as fences, power lines, trees and "
             + "the shine on cars. FSR 3.1 (AMD FidelityFX Super Resolution at native resolution) blends each "
             + "frame with the ones before it, which calms that shimmer too and gives the steadiest picture; "
             + "the price is a slightly softer image, possible faint trails behind fast-moving things, rain "
             + "and smoke, and GPU time: its pass takes about 1.4 ms a frame at 3× on an RTX 5080, about "
             + "1 ms more than MSAA (more on slower graphics cards). Off is "
             + "the fastest and the most jagged. Pick MSAA for the console's look, FSR if shimmer bothers you "
             + "more than a little softness, and Off only if your graphics card is struggling. All three "
             + "switch during play in the pause menu; FSR takes a moment to start the first time.",
        DefaultFrom = file => file.TryGetValue("mcla_msaa", out var raw) && SettingValues.Same(SettingValues.Parse(raw), false)
            ? "off" : null,
        // FSR only where its runtime for the graphics API in use was built
        // (the Vulkan one needs the Vulkan SDK at build time); otherwise the
        // game uses MSAA.
        Offered = (file, value) => !SettingValues.Same(value, "fsr") || FsrRuntimeBuilt(file),
        Unavailable = "not built for this graphics API: the game uses MSAA",
    };
    static bool FsrRuntimeBuilt(IReadOnlyDictionary<string, string> file)
    {
        var api = file.TryGetValue("mcla_gpu_backend", out var raw) ? SettingValues.Parse(raw) : "any";
        var dll = SettingValues.Same(api, "vulkan") ? "amd_fidelityfx_vk.dll" : "amd_fidelityfx_dx12.dll";
        return File.Exists(Path.Combine(Paths.BuildDir, dll));
    }
    // temporal_aa_sharpness (the SDK's): FSR's own sharpening (RCAS) after it.
    public static readonly GameSetting FsrSharpness = new("temporal_aa_sharpness", "FSR sharpness",
        [new("Off", 0.0), new("0.2 (default)", 0.2), new("0.4", 0.4), new("0.6", 0.6), new("0.8", 0.8),
         new("1.0 (sharpest)", 1.0)], 0.2)
    {
        Info = "Only with FSR anti-aliasing: how much FSR sharpens its picture. Its blending of frames softens "
             + "fine detail a little, and sharpening brings it back. 0.2 (the default) is mild; higher looks "
             + "crisper, but makes edges, noise and any trails stand out more; Off shows FSR as it is. Changes "
             + "during play in the pause menu.",
    };
    // With MSAA, the scene drawn once instead of in the Xbox 360's two
    // tiles, in the larger EDRAM the game sets up on PC (src/render_perf.cpp);
    // it changes during play too. It does nothing with anti-aliasing off or
    // FSR (one tile anyway); edram_tiles = 2048 in mcla.toml goes back to the
    // console's EDRAM altogether.
    public static readonly GameSetting SingleTile = new("mcla_single_tile", "Single tile rendering",
        [new("On (with MSAA: 9 to 20% faster at 3×)", true), new("Off (two tiles, as on the console)", false)], true)
    {
        Info = "Only matters with MSAA anti-aliasing: draws each frame's 3D scene in one go instead of in two "
             + "halves as the Xbox 360 did, which is 9 to 20% faster at 3×. Keep it on; turn it off only for "
             + "troubleshooting. With FSR or anti-aliasing off the scene is drawn in one go anyway, so it "
             + "makes no difference.",
    };
    // 60 a second by default since 2026-10-16 (the user's choice after
    // playing at 30 and 60; src/frame_timing.cpp).
    public static readonly GameSetting HudRate = new("mcla_ui_hud_rate", "HUD updates",
        [new("30 per second (as on the console)", 30L), new("60 per second", 60L),
         new("120 per second", 120L), new("Every frame", 0L)], 60L)
    {
        Info = "How many times a second the speedometer and the song title move. 60 (the default) gives smooth "
             + "needles; 30 is the console's pace, with the needles in steps and the song title scrolling at "
             + "its original speed. At 60 and 120 the title scrolls two and four times as fast. Every frame "
             + "follows the frame rate: the smoothest needles, with the title scrolling faster still at high "
             + "frame rates. Menus and pop-ups keep the console's pace whatever you pick.",
    };
    public static readonly GameSetting GraphicsApi = new("mcla_gpu_backend", "Graphics API",
        [new("Automatic (Direct3D 12 first)", "any"), new("Direct3D 12", "d3d12"), new("Vulkan", "vulkan")], "any")
    {
        Info = "Which graphics system the game uses; Automatic tries Direct3D 12 first. On the PC it was "
             + "measured on, the two are within a few percent in the city, and Direct3D 12 is 10 to 15% faster "
             + "in races at 3×. Try Vulkan if Direct3D 12 gives you trouble. Takes effect the next time the "
             + "game starts.",
    };
    public static readonly GameSetting Intro = new("mcla_intro", "Intro",
        [new("Normal", "normal"), new("Fast", "fast"), new("Skip (hides the title logo)", "skip")], "normal")
    {
        Info = "What plays when the game starts. Normal is as on the console; Fast speeds through the start-up "
             + "logos; Skip leaves them out and also hides the title logo. Takes effect the next time the game "
             + "starts.",
    };
    public static readonly GameSetting Prompts = new("mcla_button_prompts", "Button prompts",
        [new("Follow the controller", "auto"), new("Xbox", "xbox"), new("PlayStation", "playstation")], "auto")
    {
        Info = "Which button symbols the game shows. Follow the controller matches the controller you're "
             + "using; Xbox or PlayStation always shows that one.",
    };

    // HDR output (the SDK's presenter; the game's scene in HDR from
    // rexglue-hdr-scene-mcla.patch). Auto, the game's default on Windows
    // (src/mcla_app.cpp, ApplyPcDefaults), turns it on when Windows has HDR
    // on for the display; the levels' Auto is what Windows reports (the HDR
    // Calibration app's peak, the SDR content brightness slider). All change
    // during play too, in the pause menu's DISPLAY settings.
    public static readonly GameSetting Hdr = new("hdr", "HDR",
        [new("Off", "off"), new("Auto (when Windows HDR is on)", "auto"), new("On", "on")], "auto")
    {
        Info = "Shows the game in HDR on an HDR display: lights, the sun and skies go brighter than white, "
             + "while the HUD and menus stay at your desktop's brightness. Auto (recommended) turns it on "
             + "whenever Windows has HDR on for the display, and keeps SDR otherwise. On also tries when "
             + "Windows says the display isn't in HDR mode; Off always keeps SDR.",
    };
    // The brightest highlights of the scene reach about 90% of the peak (the
    // shoulder's top meets the presenter's roll-off from 0.75 of it).
    public static readonly LevelSetting HdrPeak = new("hdr_peak_nits", "HDR peak brightness", 200, 4000, 50)
    {
        Info = "The brightest your display can show. Auto uses what Windows reports for the display (the "
             + "Windows HDR Calibration app's result, if you've run it), or 1,000 nits if it reports nothing. "
             + "If bright highlights look flat or clipped, set your display's real peak: add "
             + "--hdr_test_pattern=true in More mcla.exe options (or hdr_test_pattern = true in mcla.toml), "
             + "and the game shows bars that double in brightness; where they stop getting brighter is your "
             + "peak. Remove it when you're done. The brightest parts of the scene reach about 90% of this, "
             + "which leaves room for bloom.",
        Partner = "hdr_paper_white_nits",
        PartnerBelow = true,
    };
    public static readonly LevelSetting HdrPaperWhite = new("hdr_paper_white_nits", "HDR paper white", 80, 480, 10)
    {
        Info = "How bright the game's white is, the HUD and menus included. Auto uses Windows' SDR content "
             + "brightness slider (Settings > Display > HDR), so the game matches your desktop. Set it so the "
             + "game's white matches a white window on your desktop.",
        Partner = "hdr_peak_nits",
    };
    // Brighter by default since 2026-10-16 (the look the user signed off).
    public static readonly GameSetting HdrHighlights = new("mcla_hdr_highlights", "HDR highlights",
        [new("Normal", 1.0), new("Bright", 1.5), new("Brighter", 2.0), new("Brightest", 3.0)], 2.0)
    {
        Info = "How far above the game's white bright things go: lights at night, the sun, a dusk sky. It's a "
             + "matter of taste: Brighter is the default, and Normal is the mildest.",
    };

    // frame_generation (the SDK's, rexglue-frame-generation.patch): AMD FSR 3.1
    // puts one generated frame between each two the game renders. It changes
    // during play, on both graphics APIs, in the pause menu too. Offered where
    // the FidelityFX runtime for the graphics API was built (the same as FSR's;
    // the game itself says in its menu when the graphics card can't run it).
    public static readonly GameSetting FrameGeneration = new("frame_generation", "Frame generation",
        [new("Off", "off"), new("FSR 3.1", "fsr")], "off")
    {
        Info = "Shows one generated frame between each pair the game renders, so 120 FPS looks like 240. It "
             + "needs a variable-refresh display (G-Sync or FreeSync) or one of 120 Hz or more, and works best "
             + "from 60 FPS rendered (80 or more for racing). It adds input lag (6.3 ms from the game's swap to "
             + "the screen at 116 rendered on a 240 Hz display, against 2.1 ms without it) and some GPU time "
             + "(about 0.4 ms at native resolution, 1.3 ms at 3x, on an RTX 5080). The frame rate setting "
             + "becomes the rendered rate, by default Half display, a little under half your refresh rate. "
             + "The HUD and menus are put over each generated frame as they are, not blended. It takes effect "
             + "during play on both graphics "
             + "APIs (the pause menu has the same setting); the game says there when your graphics card or "
             + "driver can't run it, and plays without it.",
        Offered = (file, value) => !SettingValues.Same(value, "fsr") || FsrRuntimeBuilt(file),
        Unavailable = "not built for this graphics API",
    };

    /// Whether the file has frame generation on, and it can be (its runtime built).
    public static bool FrameGenerationOn(IReadOnlyDictionary<string, string> file) =>
        file.TryGetValue(FrameGeneration.Key, out var raw) && SettingValues.Same(SettingValues.Parse(raw), "fsr")
        && FsrRuntimeBuilt(file);

    /// HALF DISPLAY (src/frame_timing.cpp): floor of 97% of half the refresh rate, rendered, on a display of
    /// 100 Hz or more; 0 where it isn't offered.
    public static int HalfDisplayCap(double refreshHz) =>
        refreshHz >= 100 ? (int)Math.Floor(refreshHz * 0.97 / 2) : 0;

    // mcla_fps_frame_generation (src/frame_timing.cpp): the cap with frame generation on, the
    // rendered rate. Half display, just under half the refresh rate, is the default, the one that keeps
    // the presents within what the display takes. mcla_fps is the player's own cap for when it is off.
    static GameSetting FrameRateWithFrameGeneration(int half) => new("mcla_fps_frame_generation",
        "Frame rate with frame generation",
        [new($"Half display ({half} FPS, {2 * half} shown on your display)", -1L),
         new("30 FPS (60 shown)", 30L), new("40 FPS (80 shown)", 40L), new("45 FPS (90 shown)", 45L),
         new("50 FPS (100 shown)", 50L), new("60 FPS (120 shown)", 60L), new("120 FPS (240 shown)", 120L),
         new("Uncapped", 0L)], -1L)
    {
        Info = "The frame-rate cap while frame generation is on, counted in rendered frames: 60 shows 120. Half "
             + "display (the default) is a little under half your display's refresh rate, so the presents stay "
             + "just under what the display takes and the delay from the game to the screen stays short "
             + "(about 6 ms at 116 on a 240 Hz display). A higher cap is allowed but adds latency: frames "
             + "queue behind the display's own limit (about 22 ms at 120 rendered on 240 Hz). A cap below "
             + "half your refresh rate is fine too, and best from 60 up (30 to 45 look choppy between the "
             + "frames). The frame rate above is the cap while frame generation is off.",
    };

    /// The frame rate setting to show: with frame generation on, on a display of 100 Hz or more, the
    /// one with Half display (the cap of its own); otherwise the player's cap. An old cap in the file stays as
    /// it is, used again whenever frame generation is off.
    public static GameSetting FrameRateFor(IReadOnlyDictionary<string, string> file, double refreshHz) =>
        FrameGenerationOn(file) && HalfDisplayCap(refreshHz) > 0
            ? FrameRateWithFrameGeneration(HalfDisplayCap(refreshHz))
            : FrameRate;

    // low_latency (the SDK's, rexglue-low-latency.patch): the GPU vendor's latency reducer, NVIDIA
    // Reflex or AMD Anti-Lag 2, named in plain text by the GPU the game will most likely use (the
    // one it used at its last start, from its shader record; before that the first adapter
    // Windows lists, and on a laptop with two GPUs the game may pick the other). Default is on for NVIDIA Reflex and off for the rest (Anti-Lag 2 is
    // untested). Frame generation turns it on whatever this says (the game's
    // low_latency_frame_generation, which only mcla.toml sets); it changes during play too.
    public static GameSetting LowLatencyFor(uint gpuVendor)
    {
        const uint Nvidia = 0x10DE, Amd = 0x1002;
        var name = gpuVendor switch { Nvidia => "NVIDIA Reflex", Amd => "AMD Anti-Lag 2", _ => "Low latency" };
        var choices = new List<SettingChoice>
        {
            new(gpuVendor == Nvidia ? "Default (on)" : "Default (off)", "auto"), new("Off", "off"), new("On", "on"),
        };
        if (gpuVendor == Nvidia) choices.Add(new("On + Boost (the GPU clocks stay high)", "boost"));
        var info = gpuVendor switch
        {
            Nvidia => "NVIDIA's latency reducer, on by default. The game waits at the start of each frame, at the "
                    + "moment the driver picks, so that the frame meets the GPU just as it is free and the queue of "
                    + "frames waiting for it (and the input lag that adds) goes away. It matters where the GPU is the "
                    + "limit, as at 3x resolution, where it took 13 to 15 ms off the time from the game's swap to the "
                    + "screen (RTX 5080); where the GPU has room it changes nothing. It costs about 4% of the frame "
                    + "rate at 3x on Direct3D 12 (none on Vulkan or at native resolution): turn it off if you would "
                    + "rather have the frames. With a frame-rate cap it also holds the cap, which shows the input 8 to "
                    + "14 ms newer. On + Boost also keeps the GPU's clocks high (more power). Frame generation turns it "
                    + "on whatever this says. Needs a GeForce driver with Reflex.",
            Amd => "AMD's latency reducer (Radeon RX 5000 series and later, a recent Adrenalin driver), off by "
                 + "default. The game waits at the start of each frame, at the moment the driver picks, so that the "
                 + "queue of frames waiting for the GPU (and the input lag that adds) goes away. Frame generation "
                 + "turns it on whatever this says. Untested: written from AMD's documentation, never run on a "
                 + "Radeon.",
            _ => "The GPU's latency reducer (NVIDIA Reflex, AMD Anti-Lag 2). The game found none for this GPU "
               + "(Intel has no equivalent the game can use), so this changes nothing.",
        };
        return new("low_latency", name, [.. choices], "auto") { Info = info };
    }

    public static GameSetting[] GraphicsFor(IReadOnlyDictionary<string, string> file, double refreshHz,
                                            uint gpuVendor = 0) =>
        [Resolution, FrameRateFor(file, refreshHz), Display, DisplayGamma, AntiAliasing, FrameGeneration,
         LowLatencyFor(gpuVendor), FsrSharpness, SingleTile, HudRate, GraphicsApi];
    public static readonly GameSetting[] HdrChoices = [Hdr, HdrHighlights];
    public static readonly LevelSetting[] HdrLevels = [HdrPeak, HdrPaperWhite];
    public static readonly GameSetting[] Game = [Intro, Prompts];

    // The launcher's own options (Advanced).
    public const string ExtraArgumentsInfo =
        "Extra options passed to the game every time it starts, for troubleshooting or settings the launcher "
        + "doesn't show, such as --log_file=run.log. They win over everything above and over the pause menu. "
        + "An option typed twice counts once, with its last value. Leave it empty unless you need something specific.";
    public const string CheckForUpdatesInfo =
        "Looks for a new version of the fork on GitHub when the launcher opens. It downloads nothing by "
        + "itself; updating, which rebuilds the game in a few minutes, is up to you.";
    public const string CloseOnLaunchInfo =
        "Closes this window once the game is running. Turn it off to keep the launcher open while you play; "
        + "its settings are locked until the game closes.";
    public const string AutoPlayInfo =
        "Starts the game a few seconds after the launcher opens, when it's up to date, with a countdown you "
        + "can cancel.";

    /// Started by the launcher, from its own shortcut, or again by its pause
    /// menu's RESTART GAME: any mcla.exe counts, as for the build.
    public static bool GameIsRunning()
    {
        var processes = Process.GetProcessesByName("mcla");
        foreach (var process in processes) process.Dispose();
        return processes.Length > 0;
    }
}

/// The player's name: the profile's gamertag, which the game shows (the SDK's user_name cvar,
/// patches/rexglue-user-name.patch). The SDK takes 1 to 15 ASCII letters, digits and spaces (the
/// spaces around it dropped) and otherwise "User", as before the name could be set; the launcher
/// lets only such a name through. It is a line in mcla.toml like the game's other settings, and the
/// game reads it when it starts. The profile's number (the XUID), so the folder of the save and
/// the achievements, doesn't depend on it.
static class PlayerName
{
    public const string Key = "user_name";
    public const string Default = "User";
    public const int MaxLength = 15;
    public const string Info =
        "The name the game shows for you (the profile's gamertag): up to 15 letters, digits and spaces, without "
        + "accents. Empty is \"User\", as before this setting. Takes effect the next time the game starts. Your "
        + "save and achievements stay as they are whatever the name: they belong to the profile, not to its name. "
        + "A name the game can't take is ignored and the game says \"User\".";

    /// What can be typed: ASCII letters, digits and spaces, 15 at most, spaces and all (a space typed last
    /// is the start of the next word).
    public static string Clean(string? text)
    {
        var sb = new StringBuilder();
        foreach (var c in text ?? "")
            if ((char.IsAsciiLetterOrDigit(c) || c == ' ') && sb.Length < MaxLength) sb.Append(c);
        return sb.ToString();
    }

    /// What goes into mcla.toml: the cleaned name without the spaces around it; null (no line, the game
    /// says "User") when nothing is left.
    public static string? ForFile(string? text)
    {
        var name = Clean(text).Trim();
        return name.Length == 0 ? null : name;
    }

    /// The name the file has, "" while it has none.
    public static string In(IReadOnlyDictionary<string, string> file) =>
        file.TryGetValue(Key, out var raw) ? SettingValues.Display(SettingValues.Parse(raw)) : "";
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

    /// Removes these top-level keys from the file (nothing when it isn't there or has none of them). Throws, leaving
    /// the file as it was, when it can't be read as UTF-8 or written.
    public static void Remove(string path, params string[] keys)
    {
        if (!File.Exists(path)) return;
        string content;
        try
        {
            content = new UTF8Encoding(false, throwOnInvalidBytes: true).GetString(File.ReadAllBytes(path));
        }
        catch (DecoderFallbackException)
        {
            throw new IOException($"{path} isn't UTF-8 text; it was left as it is");
        }
        var text = Split(content);
        var statements = Scan(text.Lines, out _);
        var any = false;
        for (var k = statements.Count - 1; k >= 0; k--)
        {
            var s = statements[k];
            if (s.Table.Length != 0 || !keys.Contains(s.Key)) continue;
            text.Lines.RemoveRange(s.First, s.Last - s.First + 1);
            any = true;
        }
        if (any) WriteDurably(path, new UTF8Encoding(false).GetBytes(Join(text)));
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
    // Written as mcla_anti_aliasing (the old launcher's On and Off).
    static readonly object?[] OldAntiAliasing = [null, "msaa", "off"];
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

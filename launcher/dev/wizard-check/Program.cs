// Development only: the setup wizard's engine (launcher/src/Setup) without its window.
//   xdvdfs <image.iso> <game-files.json>
//       the disc image's files against the fingerprints, each file hashed through the reader
//   verdicts <image.iso> <game folder> <game-files.json> <scratch>
//       the check on the real image and folder, and on altered copies of them made in <scratch> (default.xex
//       and the small files copied, the big ones sparse placeholders of the right size): each verdict
//   copy <image.iso> <destination> <game-files.json> [cancel after ms]
//       the copy with hashes; with a time, cancelled then run again (resume), and a copy from a damaged
//       source (sparse placeholders) which must be refused
//   release <folder>
//       a release source folder: release.json parsed and validated, and altered copies refused
//   downloads <scratch>
//       checked downloads: a wrong hash or size is refused and deleted
//   budget
//       the folder rules (PathBudget)
//   mode <scratch>
//       which of Classic, Installed and Wizard starts, in each layout (Paths.Detect)
//   cleanup <scratch>
//       the removal of an install: only what the setup made, never through a link, never the game's own folder
//   uninstall <install root> [<exe folder>]
//       InstallCleanup.Uninstall on a real install
//   install <release folder> <install root> <game: .iso, folder or "adopt"> [--in-place] [--args "<game options>"] [--exe-dir <folder>]
//       the whole setup (SetupEngine), its steps printed as they change; "adopt" takes the game the folder was set up with
//   git                       (Checks2.cs) git found and run in Installed mode, from a copy of this program with a pointer beside it
//   lock <scratch>            one setup at a time in a folder
//   orphan <pid file>         a child in the job outlives its parent? (it must not)
//   retry <scratch>           renames retried while a scanner holds the file
//   archive <scratch>         odd names in archives, the record of an unpacked tool
//   names <release folder>    release.json that names what can't be used
//   nonhttps <scratch>        what DownloadAsync refuses
//   remaining <scratch>       what is left to do, for the disk preflight
//   cleanup2 <scratch>        what an uninstall refuses
using System.Text.Json;
using MclaLauncher.Setup;

namespace MclaLauncher;

static partial class Program
{
    static int failures;

    static void Check(string what, bool ok, string detail = "")
    {
        Console.WriteLine($"{(ok ? "ok  " : "FAIL")} {what}{(detail.Length > 0 ? $": {detail}" : "")}");
        if (!ok) failures++;
    }

    static GameFingerprint LoadFingerprint(string file)
    {
        var doc = JsonSerializer.Deserialize<ReleaseGame>(File.ReadAllText(file), ReleaseJson.Options)!;
        return doc.ToFingerprint();
    }

    static async Task<int> Main(string[] args)
    {
        if (args.Length == 0) { Console.WriteLine("see Program.cs"); return 2; }
        try
        {
            switch (args[0])
            {
                case "xdvdfs": await Xdvdfs(args[1], LoadFingerprint(args[2])); break;
                case "verdicts": Verdicts(args[1], args[2], LoadFingerprint(args[3]), args[4]); break;
                case "copy": await Copy(args[1], args[2], LoadFingerprint(args[3]), args.Length > 4 ? int.Parse(args[4]) : 0); break;
                case "release": await Release(args.Length > 1 ? args[1] : null); break;
                case "downloads": await Downloads(args[1]); break;
                case "budget": Budget(); break;
                case "cleanup": Cleanup(args[1]); break;
                case "mode": Mode(args[1]); break;
                case "uninstall": { var r = InstallCleanup.Uninstall(args[1], args.Length > 2 ? args[2] : null); Console.WriteLine($"{r.Summary}\nremoved: {string.Join(", ", r.Removed)}\nkept: {string.Join(", ", r.Kept)}"); break; }
                case "install": return await Install(args[1..]);
                case "git": await GitInInstalledMode(); break;
                case "lock": LockChecks(args[1]); break;
                case "orphan": Orphan(args[1], args.Length < 3 || args[2] != "nojob"); break;
                case "retry": RetryChecks(args[1]); break;
                case "archive": await ArchiveChecks(args[1]); break;
                case "names": ReleaseNameChecks(args[1]); break;
                case "nonhttps": await NonHttpsChecks(args[1]); break;
                case "remaining": RemainingChecks(args[1]); break;
                case "cleanup2": CleanupChecks(args[1]); break;
                default: Console.WriteLine($"unknown command {args[0]}"); return 2;
            }
        }
        catch (SetupException ex)
        {
            Console.WriteLine($"SetupException: {ex.Plain}\n{ex.Detail}");
            return 1;
        }
        Console.WriteLine(failures == 0 ? "all ok" : $"{failures} FAILED");
        return failures == 0 ? 0 : 1;
    }

    // -------------------------------------------------------------------------------------------

    static async Task Xdvdfs(string iso, GameFingerprint expected)
    {
        var clock = System.Diagnostics.Stopwatch.StartNew();
        using var image = XboxDiscImage.Open(iso);
        Check("the image opens", true, $"{image.Entries.Count} files in {clock.ElapsedMilliseconds} ms");
        Check("same file names as the fingerprints", image.Entries.Select(e => e.Path.TrimStart('/')).OrderBy(n => n, StringComparer.Ordinal)
              .SequenceEqual(expected.Files.Select(f => f.Path).OrderBy(n => n, StringComparer.Ordinal)));
        foreach (var file in expected.Files)
        {
            var entry = image.Entries.FirstOrDefault(e => e.Path == "/" + file.Path);
            Check($"{file.Path} size", entry?.Size == file.Size, $"{entry?.Size} vs {file.Size}");
        }
        clock.Restart();
        using var source = new DiscSource(iso);
        var progress = new Progress<CopyProgress>(_ => { });
        await GameCopier.VerifyAsync(source, expected, progress, CancellationToken.None);
        Check("every file's SHA-256 read through the reader matches", true, $"{expected.TotalSize / 1e9:0.00} GB in {clock.Elapsed.TotalSeconds:0.0} s");
    }

    // -------------------------------------------------------------------------------------------

    static void MakeSparse(string path, long size)
    {
        using var f = new FileStream(path, FileMode.Create, FileAccess.Write);
        f.SetLength(size);
    }

    /// A folder like the real game's: default.xex, nxeart and the pad copied from `real`, the others empty placeholders of the right size.
    static string MakeFolder(string scratch, string name, string real, GameFingerprint expected, Action<string>? mutate = null)
    {
        var dir = Path.Combine(scratch, name);
        SafeFolder.Delete(dir);
        Directory.CreateDirectory(dir);
        foreach (var file in expected.Files)
        {
            var target = Path.Combine(dir, file.Path);
            if (file.Size < 20_000_000) File.Copy(Path.Combine(real, file.Path), target);
            else MakeSparse(target, file.Size);
        }
        mutate?.Invoke(dir);
        return dir;
    }

    static void Verdicts(string iso, string real, GameFingerprint expected, string scratch)
    {
        Directory.CreateDirectory(scratch);
        void Expect(string what, string path, GameVerdict verdict)
        {
            var result = GameChecker.Inspect(path, expected, out var source);
            source?.Dispose();
            Check($"{what} -> {verdict}", result.Verdict == verdict, $"\"{result.Headline}\" {result.Detail}");
        }
        Expect("the real disc image", iso, GameVerdict.Ok);
        Expect("the real extracted folder", real, GameVerdict.Ok);
        Expect("a folder like it (placeholders)", MakeFolder(scratch, "sparse-good", real, expected), GameVerdict.Ok);
        Expect("another version in default.xex", MakeFolder(scratch, "other-version", real, expected, d =>
        {
            var bytes = File.ReadAllBytes(Path.Combine(d, "default.xex"));
            uint count = System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(0x14));
            for (uint i = 0; i < count; i++)
            {
                if (System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan((int)(0x18 + i * 8))) != 0x00040006) continue;
                uint at = System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan((int)(0x1C + i * 8)));
                System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan((int)at + 4), 9);
            }
            File.WriteAllBytes(Path.Combine(d, "default.xex"), bytes);
        }), GameVerdict.AnotherRelease);
        Expect("a changed byte in default.xex (same version)", MakeFolder(scratch, "xex-byte", real, expected, d =>
        {
            using var f = new FileStream(Path.Combine(d, "default.xex"), FileMode.Open, FileAccess.ReadWrite);
            f.Seek(0x100000, SeekOrigin.Begin); var b = f.ReadByte(); f.Seek(0x100000, SeekOrigin.Begin); f.WriteByte((byte)(b ^ 1));
        }), GameVerdict.DamagedDump);
        Expect("another title id in default.xex", MakeFolder(scratch, "other-title", real, expected, d =>
        {
            var bytes = File.ReadAllBytes(Path.Combine(d, "default.xex"));
            var info = XexInfo.Read(bytes)!;
            // The title id sits 12 bytes into the execution info the header points at.
            uint count = System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan(0x14));
            for (uint i = 0; i < count; i++)
            {
                if (System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan((int)(0x18 + i * 8))) != 0x00040006) continue;
                uint at = System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(bytes.AsSpan((int)(0x1C + i * 8)));
                System.Buffers.Binary.BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan((int)at + 12), 0x4D5307E6);
            }
            File.WriteAllBytes(Path.Combine(d, "default.xex"), bytes);
        }), GameVerdict.AnotherGame);
        Expect("a file missing", MakeFolder(scratch, "missing", real, expected, d => File.Delete(Path.Combine(d, "intro720.bik"))), GameVerdict.DamagedDump);
        Expect("a file of the wrong size", MakeFolder(scratch, "size", real, expected, d => MakeSparse(Path.Combine(d, "attract720.bik"), 63168256 + 1)), GameVerdict.DamagedDump);
        Expect("no default.xex", MakeFolder(scratch, "no-xex", real, expected, d => File.Delete(Path.Combine(d, "default.xex"))), GameVerdict.NotAGame);
        Expect("default.xex that is a text file", MakeFolder(scratch, "text-xex", real, expected, d => File.WriteAllText(Path.Combine(d, "default.xex"), "hello")), GameVerdict.NotAGame);
        var empty = Path.Combine(scratch, "empty"); Directory.CreateDirectory(empty);
        Expect("an empty folder", empty, GameVerdict.NotAGame);
        var text = Path.Combine(scratch, "not-an-iso.iso"); File.WriteAllText(text, new string('x', 100000));
        Expect("a text file named .iso", text, GameVerdict.NotAGame);
        var cut = Path.Combine(scratch, "cut.iso");
        using (var from = new FileStream(iso, FileMode.Open, FileAccess.Read))
        using (var to = new FileStream(cut, FileMode.Create))
        {
            var buffer = new byte[1 << 20];
            long left = 300L << 20;
            int read;
            while (left > 0 && (read = from.Read(buffer, 0, (int)Math.Min(buffer.Length, left))) > 0) { to.Write(buffer, 0, read); left -= read; }
        }
        Expect("a disc image cut after 300 MB", cut, GameVerdict.NotAGame);
        Expect("a path that doesn't exist", Path.Combine(scratch, "nothing-here"), GameVerdict.NotAGame);
    }

    // -------------------------------------------------------------------------------------------

    static async Task Copy(string iso, string destination, GameFingerprint expected, int cancelAfter)
    {
        SafeFolder.Delete(destination);
        using var source = new DiscSource(iso);
        var last = "";
        var progress = new Progress<CopyProgress>(p => { if (p.File != last) { last = p.File; Console.WriteLine($"  {p.File}  {p.Done / 1e6:0} of {p.Total / 1e6:0} MB"); } });
        var clock = System.Diagnostics.Stopwatch.StartNew();
        if (cancelAfter > 0)
        {
            using var cts = new CancellationTokenSource(cancelAfter);
            try
            {
                await GameCopier.CopyAsync(source, expected, destination, progress, cts.Token);
                Check("the first run was cancelled", false, "it finished before the time");
            }
            catch (OperationCanceledException)
            {
                var kept = expected.Files.Count(f => File.Exists(Path.Combine(destination, f.Path)));
                Check("cancelled mid-copy", true, $"{kept} files complete, {Directory.GetFiles(destination, "*.part").Length} partial");
            }
            clock.Restart();
        }
        await GameCopier.CopyAsync(source, expected, destination, progress, CancellationToken.None);
        Check("the copy finishes", GameCopier.IsComplete(destination, expected), $"{clock.Elapsed.TotalSeconds:0.0} s");
        foreach (var file in expected.Files)
            Check($"{file.Path} on disk is the fingerprint's", await Verified.IsAsync(Path.Combine(destination, file.Path), file.Size, file.Sha256, CancellationToken.None));

        // A damaged source (right names and sizes, wrong contents) is refused at the first file that differs.
        var bad = Path.Combine(Path.GetDirectoryName(destination.TrimEnd('\\'))!, "damaged-source");
        var real = Path.Combine(destination);
        MakeFolderFromCopy(bad, real, expected);
        using var badSource = GameSource.Open(bad);
        var target = destination + "-from-damaged";
        SafeFolder.Delete(target);
        try
        {
            await GameCopier.CopyAsync(badSource, expected, target, progress, CancellationToken.None);
            Check("a damaged source is refused", false);
        }
        catch (GameFileException ex)
        {
            Check("a damaged source is refused", true, ex.Plain);
            Check("no bad file is left behind", !Directory.EnumerateFiles(target, "*", SearchOption.AllDirectories).Any(f => f.EndsWith(".part")));
        }
        SafeFolder.Delete(bad); SafeFolder.Delete(target);
    }

    static void MakeFolderFromCopy(string bad, string real, GameFingerprint expected)
    {
        SafeFolder.Delete(bad);
        Directory.CreateDirectory(bad);
        foreach (var file in expected.Files)
        {
            if (file.Size < 20_000_000 && file.Path != "nxeart") File.Copy(Path.Combine(real, file.Path), Path.Combine(bad, file.Path));
            else MakeSparse(Path.Combine(bad, file.Path), file.Size);
        }
    }

    // -------------------------------------------------------------------------------------------

    static async Task Release(string? folder)
    {
        if (folder == null)
        {
            // The real source: GitHub's latest release of the fork (none is published until the first real release).
            var real = ReleaseSource.FromOverride(null);
            try
            {
                var latest = await real.LoadAsync(CancellationToken.None);
                Check("GitHub's latest release is read", true, $"{latest.Tag} {latest.Commit[..8]}");
            }
            catch (SetupException ex) { Check("GitHub has no release yet, and the message says so", ex.Plain.Contains("no release"), $"{ex.Plain} ({ex.Detail})"); }
            return;
        }
        var source = ReleaseSource.FromOverride(folder);
        var release = await source.LoadAsync(CancellationToken.None);
        Check("release.json is read", true, $"{release.Tag} {release.Commit[..8]}, bundle {release.SdkBundle!.Asset}, {release.Game!.Files.Count} game files, git {source.GitUrl}");
        Check("the tools are named", ReleaseInfo.RequiredTools.All(release.Tools.ContainsKey));
        var good = File.ReadAllText(Path.Combine(folder, "release.json"));
        void Refuse(string what, Func<string, string> alter)
        {
            try { ReleaseInfo.Parse(alter(good)); Check($"refused: {what}", false, "accepted"); }
            catch (SetupException ex) { Check($"refused: {what}", true, ex.Plain); }
        }
        Refuse("another schema", j => j.Replace("\"schema\": 1", "\"schema\": 2"));
        Refuse("a short commit", j => j.Replace(release.Commit, release.Commit[..12]));
        Refuse("no game list", j => System.Text.RegularExpressions.Regex.Replace(j, "\"game\": \\{.*?\\n  \\},", "\"game\": null,", System.Text.RegularExpressions.RegexOptions.Singleline));
        Refuse("a file name that leaves the folder", j => j.Replace("\"path\": \"default.xex\"", "\"path\": \"..\\\\default.xex\""));
        Refuse("a tool missing", j => j.Replace("\"ninja\"", "\"ninjaa\""));
        Refuse("not json", _ => "<html>");
        try { ReleaseSource.FromOverride("https://example.com/"); Check("a web address as the override is refused", false); }
        catch (SetupException) { Check("a web address as the override is refused", true); }
    }

    // -------------------------------------------------------------------------------------------

    static async Task Downloads(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var data = new byte[3_000_000]; new Random(5).NextBytes(data);
        var source = Path.Combine(scratch, "source.bin");
        File.WriteAllBytes(source, data);
        var sha = Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(data));
        var log = new List<string>();
        Task Fetch(string dest, IProgress<long> progress, CancellationToken ct) => ReleaseSource.DownloadAsync(source, dest, progress, ct, allowLocalFile: true);
        var target = Path.Combine(scratch, "got.bin");
        await Verified.FetchAsync("a file", target, data.Length, sha, Fetch, new Progress<long>(_ => { }), log.Add, CancellationToken.None);
        Check("a right file is kept", File.Exists(target) && !File.Exists(target + ".part"));
        await Verified.FetchAsync("a file", target, data.Length, sha, Fetch, new Progress<long>(_ => { }), log.Add, CancellationToken.None);
        Check("a right file is not fetched again", log.Any(l => l.Contains("already")));
        File.Delete(target);
        try { await Verified.FetchAsync("a file", target, data.Length, new string('0', 64), Fetch, new Progress<long>(_ => { }), log.Add, CancellationToken.None); Check("a wrong hash is refused", false); }
        catch (SetupException ex) { Check("a wrong hash is refused", true, ex.Plain); Check("and nothing is left", !File.Exists(target) && !File.Exists(target + ".part")); }
        try { await Verified.FetchAsync("a file", target, data.Length + 1, sha, Fetch, new Progress<long>(_ => { }), log.Add, CancellationToken.None); Check("a wrong size is refused", false); }
        catch (Exception ex) { Check("a wrong size is refused", true, ex.Message); }
        try { await Verified.FetchAsync("a file", target, 10, sha, (d, p, c) => ReleaseSource.DownloadAsync(Path.Combine(scratch, "missing.bin"), d, p, c, allowLocalFile: true), new Progress<long>(_ => { }), log.Add, CancellationToken.None); Check("a missing file is reported", false); }
        catch (SetupException ex) { Check("a missing file is reported", true, ex.Plain); }

        // A zip that tries to write outside its folder, and the unpacking of a normal one.
        var zip = Path.Combine(scratch, "evil.zip");
        using (var z = System.IO.Compression.ZipFile.Open(zip, System.IO.Compression.ZipArchiveMode.Create))
        {
            var entry = z.CreateEntry("tool/../../escaped.txt");
            using var w = new StreamWriter(entry.Open()); w.Write("x");
        }
        try
        {
            await ToolInstaller.InstallAsync(Path.Combine(scratch, "tools"), "evil", "1", zip, "00", log.Add, new Progress<(int, int)>(), CancellationToken.None);
            Check("an archive that writes outside its folder is refused", false);
        }
        catch (SetupException ex) { Check("an archive that writes outside its folder is refused", true, ex.Plain); }
        Check("and it wrote nothing outside", !File.Exists(Path.Combine(scratch, "escaped.txt")) && !File.Exists(Path.Combine(scratch, "tools", "escaped.txt")));
        var good = Path.Combine(scratch, "good.zip");
        using (var z = System.IO.Compression.ZipFile.Open(good, System.IO.Compression.ZipArchiveMode.Create))
        {
            using (var w = new StreamWriter(z.CreateEntry("tool-1.0/bin/tool.exe").Open())) w.Write("x");
            using (var w = new StreamWriter(z.CreateEntry("tool-1.0/readme.txt").Open())) w.Write("y");
        }
        var folder = await ToolInstaller.InstallAsync(Path.Combine(scratch, "tools"), "tool", "1.0", good, "abc", log.Add, new Progress<(int, int)>(), CancellationToken.None);
        Check("a zip with one top folder keeps its name", Path.GetFileName(folder) == "tool-1.0" && File.Exists(Path.Combine(folder, "bin", "tool.exe")), $"{folder}: {string.Join(", ", Directory.EnumerateFileSystemEntries(folder, "*", SearchOption.AllDirectories).Select(e => Path.GetRelativePath(folder, e)))}");
        var again = await ToolInstaller.InstallAsync(Path.Combine(scratch, "tools"), "tool", "1.0", good, "abc", log.Add, new Progress<(int, int)>(), CancellationToken.None);
        Check("and is not unpacked again", log.Any(l => l.Contains("already unpacked")) && again == folder);
        var loose = Path.Combine(scratch, "loose.zip");
        using (var z = System.IO.Compression.ZipFile.Open(loose, System.IO.Compression.ZipArchiveMode.Create))
            using (var w = new StreamWriter(z.CreateEntry("ninja.exe").Open())) w.Write("x");
        var loosePath = await ToolInstaller.InstallAsync(Path.Combine(scratch, "tools"), "ninja", "1.13.2", loose, "def", log.Add, new Progress<(int, int)>(), CancellationToken.None);
        Check("a zip of loose files goes in tools\\<name>-<version>", Path.GetFileName(loosePath) == "ninja-1.13.2" && File.Exists(Path.Combine(loosePath, "ninja.exe")));
        // Deleting what the setup made never follows a link.
        var keep = Path.Combine(scratch, "keep"); Directory.CreateDirectory(keep); File.WriteAllText(Path.Combine(keep, "precious.txt"), "do not delete");
        var victim = Path.Combine(scratch, "victim"); Directory.CreateDirectory(victim);
        MakeJunction(Path.Combine(victim, "link"), keep);
        File.WriteAllText(Path.Combine(victim, "mine.txt"), "x");
        try
        {
            SafeFolder.Delete(victim);
            Check("deleting a folder with a link in it leaves the link's target alone", File.Exists(Path.Combine(keep, "precious.txt")) && !Directory.Exists(victim));
        }
        catch (Exception ex) { Check("deleting a folder with a link in it", false, ex.Message); }
    }

    // -------------------------------------------------------------------------------------------

    static void Budget()
    {
        Console.WriteLine($"max root length {PathBudget.MaxRootLength}");
        void Case(string path, bool problem)
        {
            var text = PathBudget.Problem(path, out _);
            Check($"\"{(path.Length > 50 ? path[..47] + "..." : path)}\"", (text != null) == problem, text ?? "fine");
        }
        Case(@"C:\MCLA", false);
        Case(@"C:\My Games v1.2_x-y", false);
        Case(@"C:\Jogos " + "\u00e9", true);
        Case(@"C:\Tom's", true);
        Case(@"C:\Tom & Co", true);
        Case(@"C:\100%", true);
        Case(@"C:\a;b", true);
        Case(@"C:\a^b", true);
        Case(@"C:\a!b", true);
        Case(@"C:\" + "\u0414\u043e\u043c", true);
        Case(@"C:\Users\someone\AppData\Local\MCLA", false);
        Case("C:\\" + new string('a', PathBudget.MaxRootLength - 3), false);
        Case("C:\\" + new string('a', PathBudget.MaxRootLength - 2), true);
        Case(@"C:\", true);
        Case("", true);
        Case("relative\\folder", true);
        Case(@"\\server\share\MCLA", true);
        Case(@"C:\Windows\MCLA", true);
        Case(@"C:\Program Files\MCLA", true);
        Case(Environment.GetFolderPath(Environment.SpecialFolder.Desktop), true);   // a folder with things in it
        var onedrive = PathBudget.Warning(@"C:\Users\someone\OneDrive\MCLA");
        Check("a OneDrive folder is warned about", onedrive != null);
        Check("another folder is not", PathBudget.Warning(@"C:\MCLA") == null);
    }

    // -------------------------------------------------------------------------------------------

    /// A directory junction (what a link in an install would be; creating one needs no privilege).
    static void MakeJunction(string link, string target)
    {
        var psi = new System.Diagnostics.ProcessStartInfo("cmd.exe") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
        link = Path.GetFullPath(link); target = Path.GetFullPath(target);
        psi.ArgumentList.Add("/c"); psi.ArgumentList.Add("mklink"); psi.ArgumentList.Add("/J"); psi.ArgumentList.Add(link); psi.ArgumentList.Add(target);
        using var process = System.Diagnostics.Process.Start(psi)!;
        process.StandardOutput.ReadToEnd();
        process.WaitForExit();
        if (!Directory.Exists(link)) throw new IOException($"could not make a junction at {link}");
    }

    static void Mode(string scratch)
    {
        SafeFolder.Delete(scratch);
        void Touch(string file) { Directory.CreateDirectory(Path.GetDirectoryName(file)!); File.WriteAllText(file, ""); }
        void Expect(string what, string exeDir, LaunchMode mode, string? root = null)
        {
            var (actual, actualRoot, launcherDir) = Paths.Detect(exeDir);
            Check(what, actual == mode && (root == null || string.Equals(actualRoot, root, StringComparison.OrdinalIgnoreCase)), $"{actual}, {actualRoot ?? "-"}, {launcherDir}");
        }

        // Classic: below a folder with build-game.ps1, with or without a pointer or an install beside the exe.
        var classic = Path.Combine(scratch, "classic", "mcla-recomp", "launcher");
        Touch(Path.Combine(classic, "build-game.ps1"));
        Directory.CreateDirectory(Path.Combine(classic, "app"));
        Expect("the exe in a checkout's launcher\\app is Classic", Path.Combine(classic, "app"), LaunchMode.Classic);
        Expect("so is the launcher\\ folder itself", classic, LaunchMode.Classic);
        var other = Path.Combine(scratch, "other-install");
        new InstallRecord { InstallRoot = other, State = "ready" }.Save(InstallRecord.PointerFile(Path.Combine(classic, "app")));
        Expect("a pointer beside a Classic exe changes nothing", Path.Combine(classic, "app"), LaunchMode.Classic);

        // Installed: a pointer to a finished install that has its checkout.
        var root = Path.Combine(scratch, "install");
        var exe = Path.Combine(scratch, "app");
        Directory.CreateDirectory(exe);
        Expect("no pointer is the Wizard", exe, LaunchMode.Wizard);
        new InstallRecord { InstallRoot = root, State = "setting-up" }.Save(InstallRecord.PointerFile(exe));
        Expect("a pointer to an unfinished setup is the Wizard (it resumes)", exe, LaunchMode.Wizard, root);
        new InstallRecord { InstallRoot = root, State = "ready" }.Save(InstallRecord.PointerFile(exe));
        Expect("a finished setup whose checkout is gone is the Wizard", exe, LaunchMode.Wizard, root);
        Touch(new InstallLayout(root).BuildScript);
        Expect("a finished setup with its checkout is Installed", exe, LaunchMode.Installed, root);
        File.WriteAllText(InstallRecord.PointerFile(exe), "{ not json");
        Expect("a pointer that can't be read is the Wizard", exe, LaunchMode.Wizard);

        // The user's own install (read only): Classic, from its launcher\app and from the exe's usual places.
        var real = @"C:\Games\Midnight Club Los Angeles\mcla-recomp\launcher";
        if (File.Exists(Path.Combine(real, "build-game.ps1")))
            Expect("the user's own install is Classic", Path.Combine(real, "app"), LaunchMode.Classic);
    }

    static void Cleanup(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var precious = Path.Combine(scratch, "precious-game");
        Directory.CreateDirectory(precious);
        File.WriteAllText(Path.Combine(precious, "default.xex"), "the player's own game");
        var root = Path.Combine(scratch, "install");
        var exe = Path.Combine(scratch, "exe");
        Directory.CreateDirectory(exe);
        foreach (var d in new[] { "mcla-recomp\\game", "mcla-recomp\\.git\\objects", "tools\\ninja-1", "downloads", "logs" })
            Directory.CreateDirectory(Path.Combine(root, d));
        File.WriteAllText(Path.Combine(root, "mcla-recomp", ".git", "objects", "readonly.pack"), "x");
        File.SetAttributes(Path.Combine(root, "mcla-recomp", ".git", "objects", "readonly.pack"), FileAttributes.ReadOnly);
        File.WriteAllText(Path.Combine(root, "mcla-recomp", "game", "default.xex"), "copied game");
        MakeJunction(Path.Combine(root, "tools", "link-to-precious"), precious);
        File.WriteAllText(Path.Combine(root, "notes-of-the-player.txt"), "mine");
        File.WriteAllText(Path.Combine(root, "install.json.123.tmp"), "left by a save that was cut short");
        var record = new InstallRecord { InstallRoot = root, State = "ready", GameMode = "in-place", GameDataRoot = precious };
        record.Save(Path.Combine(root, "install.json"));
        record.Save(InstallRecord.PointerFile(exe));
        var result = InstallCleanup.Uninstall(root, exe);
        Check("the install's own folders and files are removed", !Directory.Exists(Path.Combine(root, "mcla-recomp")) && !Directory.Exists(Path.Combine(root, "tools")) && !File.Exists(Path.Combine(root, "install.json")));
        Check("the game used where it is is untouched, also through a link inside the install", File.Exists(Path.Combine(precious, "default.xex")));
        Check("something the player put in the folder stays, and so does the folder", File.Exists(Path.Combine(root, "notes-of-the-player.txt")));
        Check("a save's leftover is removed", !File.Exists(Path.Combine(root, "install.json.123.tmp")));
        Check("the pointer beside the exe is removed", !File.Exists(InstallRecord.PointerFile(exe)));
        Console.WriteLine($"  {result.Summary}; kept: {string.Join(" | ", result.Kept)}");
        try { InstallCleanup.Uninstall(precious, null); Check("a folder that is not an install is refused", false); }
        catch (SetupException ex) { Check("a folder that is not an install is refused", true, ex.Plain); }
        Check("and nothing of it was removed", File.Exists(Path.Combine(precious, "default.xex")));
    }

    static async Task<int> Install(string[] args)
    {
        string releaseFolder = args[0], root = Path.GetFullPath(args[1]), game = args[2];
        bool inPlace = args.Contains("--in-place");
        string gameArgs = args.SkipWhile(a => a != "--args").Skip(1).FirstOrDefault() ?? "";
        string exeDir = args.SkipWhile(a => a != "--exe-dir").Skip(1).FirstOrDefault() ?? root + "-exe";
        Directory.CreateDirectory(exeDir);
        var source = ReleaseSource.FromOverride(releaseFolder);
        var release = await source.LoadAsync(CancellationToken.None);
        var problem = PathBudget.Problem(root, out var resumes);
        if (problem != null) { Console.WriteLine(problem); return 1; }
        if (game == "adopt")
        {
            var adopted = SetupEngine.FindInstalledGame(new InstallLayout(root), release.Game!.ToFingerprint());
            if (adopted == null) { Console.WriteLine("no game to adopt in that folder"); return 1; }
            Console.WriteLine($"adopting the game of the folder: {adopted.Path} (in place: {adopted.InPlace})");
            game = adopted.Path;
            inPlace = adopted.InPlace;
        }
        Console.WriteLine($"release {release.Tag} from {source.Describe}; install {root}{(resumes ? " (resuming)" : "")}");
        var engine = new SetupEngine(new SetupContext(new InstallLayout(root), source, release, new SetupChoices(root, game, inPlace, gameArgs), exeDir));
        var last = new Dictionary<StepId, string>();
        engine.Changed += () =>
        {
            foreach (var step in engine.Steps)
            {
                var line = $"{step.State,-9} {step.Title}  {(step.Fraction is double f ? $"{f * 100:0}%" : "")}  {step.Text}  {step.Remaining}";
                if (!last.TryGetValue(step.Id, out var before) || before.Split("  ")[0] != line.Split("  ")[0] || DateTime.Now.Second % 10 == 0 && before != line)
                {
                    last[step.Id] = line;
                    Console.WriteLine($"[{DateTime.Now:HH:mm:ss}] {line}");
                }
            }
        };
        using var cts = new CancellationTokenSource();
        Console.CancelKeyPress += (_, e) => { e.Cancel = true; engine.Cancel(); cts.Cancel(); };
        try
        {
            await engine.RunAsync(cts.Token);
        }
        catch (OperationCanceledException) { Console.WriteLine("cancelled"); return 3; }
        catch (SetupException ex) { Console.WriteLine($"FAILED: {ex.Plain}\n{ex.Detail}\nlog: {engine.LogFilePath}"); return 1; }
        Console.WriteLine($"done. record: {engine.Record.State}; pointer saved: {engine.PointerSaved}; log {engine.LogFilePath}");
        foreach (var step in engine.Steps) Console.WriteLine($"  {step.State,-9} {step.Title}: {step.Text}");
        // The longest path the install holds, from the install folder: PathBudget.LongestBelowRoot must cover it.
        var full = Path.GetFullPath(root).TrimEnd('\\');
        // The build's own products (mcla-recomp\out\) are left out of the measure: CMake moves an object file into a
        // short hashed folder when its path would pass 250 characters, so their length depends on the folder and
        // never decides it. Everything else is what PathBudget.LongestBelowRoot has to cover.
        var entries = Directory.EnumerateFileSystemEntries(full, "*", SearchOption.AllDirectories).ToList();
        var outFolder = Path.Combine(full, InstallLayout.RepoFolder, "out") + "\\";
        var longest = entries.Where(e => !e.StartsWith(outFolder, StringComparison.OrdinalIgnoreCase)).MaxBy(e => e.Length)!;
        var below = longest.Length - full.Length - 1;
        var longestOut = entries.MaxBy(e => e.Length)!;
        Console.WriteLine($"longest path below the install folder (without out\\): {below} characters ({longest[(full.Length + 1)..]}); PathBudget.LongestBelowRoot = {PathBudget.LongestBelowRoot}");
        Console.WriteLine($"install folder {full.Length}; longest path of all {longestOut.Length - full.Length - 1} below it, {longestOut.Length} in all (Windows' limit {PathBudget.MaxPath})");
        if (below > PathBudget.LongestBelowRoot) { Console.WriteLine("FAIL: the budget is too small"); return 4; }
        if (longestOut.Length > PathBudget.MaxPath) { Console.WriteLine("FAIL: a path of the install is over Windows' limit"); return 4; }
        return 0;
    }
}

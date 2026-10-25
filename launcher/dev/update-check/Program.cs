// Checks of the launcher's self-update pieces, on scratch folders under %TEMP% (nothing outside them is touched;
// the shortcuts of the installer scenarios go to a scratch folder too, never the desktop or the Start menu).
//
//   dotnet run --project launcher\dev\update-check -- swap
//   dotnet run --project launcher\dev\update-check -- download
//   dotnet run --project launcher\dev\update-check -- setup <path of the bare MCLA-Setup.exe>   (the installer program)
//   dotnet run --project launcher\dev\update-check -- all <path of MCLA-Setup.exe>
//
// "setup" builds fake launcher payloads (the folder holds a copy of a system program as MclaLauncher.exe) and
// composes them behind the real installer program, so the real installer runs: unpacking, the checksums, the
// swap, shortcuts, the start check, the way back. Build the installer program with:
//   dotnet publish launcher\installer\MclaSetup.csproj -c Release -o <folder>
using System.Diagnostics;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using Mcla.Updating;
using MclaLauncher;

static class Program
{
    static int failures, checks;
    static bool quiet;
    static readonly string Scratch = Path.Combine(Path.GetTempPath(), "mcla-update-check-" + Environment.ProcessId);

    static int Main(string[] args)
    {
        Directory.CreateDirectory(Scratch);
        try
        {
            var command = args.Length > 0 ? args[0] : "";
            var stub = args.Length > 1 ? args[1] : Environment.GetEnvironmentVariable("MCLA_SETUP_STUB");
            if (command is "swap" or "all") Swap();
            if (command is "download" or "all") Download().GetAwaiter().GetResult();
            if (command == "download-loop")
            {
                // The download checks over and over (a race in them, or in the test server, would show as a failure
                // or a crash somewhere in the loop): download-loop <times>
                var times = args.Length > 1 && int.TryParse(args[1], out var n) ? n : 50;
                quiet = true;
                for (var i = 1; i <= times; i++) { Download().GetAwaiter().GetResult(); if (i % 10 == 0) Console.WriteLine($"  {i} rounds, {checks - failures} of {checks} checks passed so far"); }
            }
            if (command is "setup" or "all")
            {
                if (stub == null || !File.Exists(stub)) { Console.Error.WriteLine("setup needs the path of the bare MCLA-Setup.exe"); return 2; }
                Setup(Path.GetFullPath(stub));
            }
            if (command is not ("swap" or "download" or "download-loop" or "setup" or "all")) { Console.Error.WriteLine("swap | download | download-loop <times> | setup <MCLA-Setup.exe> | all <MCLA-Setup.exe>"); return 2; }
        }
        finally
        {
            var waited = Stopwatch.StartNew();
            while (FolderSwap.ProcessesUnder(Scratch).Count > 0 && waited.Elapsed.TotalSeconds < 30) Thread.Sleep(500);
            FolderSwap.DeleteFolder(Scratch, TimeSpan.FromSeconds(10));
        }
        Console.WriteLine($"\n{checks - failures} of {checks} checks passed" + (failures > 0 ? $", {failures} FAILED" : ""));
        return failures == 0 ? 0 : 1;
    }

    static void Check(bool ok, string what)
    {
        checks++;
        if (!ok) failures++;
        if (!ok || !quiet) Console.WriteLine($"  {(ok ? "ok  " : "FAIL")} {what}");
    }

    static void Section(string name) { if (!quiet) Console.WriteLine($"\n{name}"); }

    static string Dir(string name)
    {
        var p = Path.Combine(Scratch, name);
        if (Directory.Exists(p)) FolderSwap.DeleteFolder(p, TimeSpan.FromSeconds(5));
        Directory.CreateDirectory(p);
        return p;
    }

    static string Read(string folder, string name) => File.Exists(Path.Combine(folder, name)) ? File.ReadAllText(Path.Combine(folder, name)).Trim() : "(missing)";
    static void Write(string folder, string name, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(folder, name))!);
        File.WriteAllText(Path.Combine(folder, name), text);
    }

    // ---- the folder swap ---------------------------------------------------------------------------------------------

    static void Swap()
    {
        Section("folder swap");
        var root = Dir("swap");
        var target = Path.Combine(root, "MCLA");

        // a first install is one rename
        var staged = FolderSwap.NewStagingPath(target);
        Write(staged, "version.txt", "1");
        var s1 = FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(2));
        Check(Read(target, "version.txt") == "1" && s1.Previous == null && !Directory.Exists(staged), "first install: the staged folder is the target, nothing before it");
        FolderSwap.Commit(s1);

        // a replacement keeps what the user's setup wrote (the pointer), and commit removes the old folder
        Write(target, "install.json", "{\"installRoot\":\"C:\\\\MCLA\"}");
        Write(target, "app.dll", "old");
        staged = FolderSwap.NewStagingPath(target);
        Write(staged, "version.txt", "2");
        Write(staged, "app.dll", "new");
        FolderSwap.CopyKept(target, staged, ["install.json", "missing.json", @"..\escape.txt", @"C:\Windows\win.ini"]);
        var s2 = FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(2));
        Check(Read(target, "version.txt") == "2" && Read(target, "app.dll") == "new", "replace: the new content is in place");
        Check(Read(target, "install.json").Contains("installRoot"), "replace: install.json was carried over");
        Check(s2.Previous != null && Read(s2.Previous, "version.txt") == "1", "replace: the old folder is beside it until commit");
        Check(!File.Exists(Path.Combine(root, "escape.txt")) && !File.Exists(Path.Combine(target, "win.ini")), "carry-over refuses '..' and rooted names");
        FolderSwap.Commit(s2);
        Check(!FolderSwap.Leftovers(target).Any(), "commit removes the old folder");

        // the way back
        staged = FolderSwap.NewStagingPath(target);
        Write(staged, "version.txt", "3");
        var s3 = FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(2));
        var restored = FolderSwap.RollBack(s3);
        Check(restored && Read(target, "version.txt") == "2", "rollback: version 2 is back");
        Check(!FolderSwap.Leftovers(target).Any(), "rollback: no .failed or .old folder is left");

        // a held file stops the swap before anything changes
        staged = FolderSwap.NewStagingPath(target);
        Write(staged, "version.txt", "4");
        using (var held = new FileStream(Path.Combine(target, "version.txt"), FileMode.Open, FileAccess.Read, FileShare.None))
        {
            var watch = Stopwatch.StartNew();
            Exception? error = null;
            try { FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(1.5)); } catch (Exception ex) { error = ex; }
            Check(error is IOException && watch.Elapsed.TotalSeconds >= 1.4, $"locked: gives up after the timeout ({watch.Elapsed.TotalSeconds:N1} s) with an IOException");
            Check(Directory.Exists(target) && File.Exists(Path.Combine(target, "version.txt")) && !FolderSwap.Leftovers(target).Any(d => d.Contains(FolderSwap.OldTag)), "locked: the installed folder is where it was, with no .old beside it");
        }
        Check(Directory.Exists(staged) && Read(target, "version.txt") == "2", "locked: the staged folder is still there for a retry");
        FolderSwap.DeleteFolder(staged, TimeSpan.FromSeconds(2));

        // killed between the two renames: Recover puts the old folder back
        Directory.Move(target, target + ".old-20260101-000000-1");
        Check(!Directory.Exists(target), "setup of the crash case: no target");
        Check(FolderSwap.Recover(target) && Read(target, "version.txt") == "2", "recover: the previous folder came back");
        Check(!FolderSwap.Recover(target), "recover: nothing to do when the folder is there");

        // clean-up removes only the swap's leftovers
        Directory.CreateDirectory(target + ".old-1");
        Directory.CreateDirectory(target + ".new-2");
        Directory.CreateDirectory(target + ".failed-3");
        Directory.CreateDirectory(target + ".keep");
        Directory.CreateDirectory(target + "2");
        var removed = FolderSwap.CleanLeftovers(target);
        Check(removed == 3 && Directory.Exists(target + ".keep") && Directory.Exists(target + "2") && Directory.Exists(target), "clean-up: removed 3 leftovers and nothing else");

        // processes: a program running from the folder keeps it busy
        var ping = Path.Combine(target, "ping.exe");
        File.Copy(Path.Combine(Environment.SystemDirectory, "ping.exe"), ping);
        using (var p = Process.Start(new ProcessStartInfo(ping, "-n 4 127.0.0.1") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true })!)
        {
            Thread.Sleep(300);
            var under = FolderSwap.ProcessesUnder(target);
            Check(under.Any(x => x.Id == p.Id), "processes: a program run from the folder is found");
            foreach (var x in under) x.Dispose();
            Check(!FolderSwap.WaitUntilFree(target, TimeSpan.FromMilliseconds(500)), "processes: the folder is not free while it runs");
            Check(FolderSwap.WaitUntilFree(target, TimeSpan.FromSeconds(15)), "processes: it is free once the program ended");
            Check(FolderSwap.WaitForProcess(p.Id, TimeSpan.FromSeconds(1)), "processes: waiting for a finished process returns at once");
        }
        Check(FolderSwap.WaitForProcess(int.MaxValue / 2, TimeSpan.FromSeconds(1)), "processes: a process that never existed is not waited for");
    }

    // ---- the verified download -----------------------------------------------------------------------------------------

    static readonly TimeSpan Quick = TimeSpan.FromMilliseconds(50); // the pause between download tries, short for the tests

    sealed class Server : IDisposable
    {
        HttpListener listener = new();
        public int Requests;
        public Func<HttpListenerContext, int, Task> Handler = (_, _) => Task.CompletedTask;
        public string Url { get; }
        public int Port { get; }
        readonly Task loop;
        public Server(int? firstPort = null)
        {
            var port = firstPort ?? Random.Shared.Next(20000, 60000);
            for (var tries = 0; ; tries++)
            {
                // A listener whose Start failed (the port is taken) is closed by the failure and cannot be used again:
                // each try has a new one. (This was a bug of the harness: it reused it, and a port clash showed as
                // "Cannot access a disposed object" at the next prefix.)
                listener = new HttpListener();
                try { listener.Prefixes.Add($"http://127.0.0.1:{port}/"); listener.Start(); break; }
                catch (HttpListenerException) when (tries < 50) { listener.Close(); port = port >= 65000 ? 20000 : port + 1; }
            }
            Port = port;
            Url = $"http://127.0.0.1:{port}/MCLA-Setup.exe";
            var mine = listener;
            loop = Task.Run(async () =>
            {
                while (true)
                {
                    HttpListenerContext ctx;
                    try { ctx = await mine.GetContextAsync(); } catch (Exception) { return; } // closed
                    var n = Interlocked.Increment(ref Requests);
                    _ = Task.Run(async () => { try { await Handler(ctx, n); } catch (Exception) { try { ctx.Response.Abort(); } catch (Exception) { } } });
                }
            });
        }
        public void Dispose() { try { listener.Stop(); } catch (Exception) { } listener.Close(); }
    }

    static async Task Serve(HttpListenerContext ctx, byte[] body, int? sendOnly = null, bool stallAfter = false)
    {
        ctx.Response.ContentLength64 = body.Length;
        var count = sendOnly ?? body.Length;
        await ctx.Response.OutputStream.WriteAsync(body.AsMemory(0, count));
        if (stallAfter) { await ctx.Response.OutputStream.FlushAsync(); await Task.Delay(8000); }
        if (count < body.Length) { ctx.Response.Abort(); return; }
        ctx.Response.Close();
    }

    static async Task Download()
    {
        Section("verified download");
        var data = new byte[3 * 1024 * 1024];
        new Random(7).NextBytes(data);
        var good = new UpdateAsset("MCLA-Setup.exe", "", Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant(), data.Length);
        var folder = Dir("download");

        // a test server whose first port is taken goes on to the next one (it once failed with "Cannot access a disposed
        // object": a listener whose Start failed was used again)
        using (var first = new Server())
        using (var second = new Server(first.Port))
            Check(second.Port != first.Port, $"a test server asked for a port that is taken picks another ({first.Port}, {second.Port})");

        using (var server = new Server())
        {
            var asset = good with { Url = server.Url };
            server.Handler = (ctx, n) => Serve(ctx, data);
            long lastDone = 0;
            var path = await SelfUpdate.DownloadAsync(asset, folder, (done, total) => lastDone = done, default, allowLoopbackHttp: true);
            Check(File.Exists(path) && SHA256.HashData(File.ReadAllBytes(path)).AsSpan().SequenceEqual(SHA256.HashData(data)) && lastDone == data.Length && !File.Exists(path + ".part"), "a good file arrives complete, progress reaches the end, no .part is left");

            var before = server.Requests;
            await SelfUpdate.DownloadAsync(asset, folder, null, default, allowLoopbackHttp: true);
            Check(server.Requests == before, "a file already downloaded and exact is not fetched again");
        }

        folder = Dir("download-hash");
        using (var server = new Server())
        {
            var wrong = (byte[])data.Clone();
            wrong[1000] ^= 0xFF; // same size, one byte different
            server.Handler = (ctx, n) => Serve(ctx, wrong);
            var error = await Catch(() => SelfUpdate.DownloadAsync(good with { Url = server.Url }, folder, null, default, allowLoopbackHttp: true));
            Check(error is UpdateException e && e.Message.Contains("damaged"), "a wrong SHA-256 is refused");
            Check(server.Requests == 1, "a wrong SHA-256 is not retried");
            Check(Directory.GetFiles(folder).Length == 0, "nothing is left behind after a wrong SHA-256");
        }

        folder = Dir("download-size");
        using (var server = new Server())
        {
            var longer = data.Concat(new byte[10]).ToArray();
            server.Handler = (ctx, n) => Serve(ctx, longer);
            var error = await Catch(() => SelfUpdate.DownloadAsync(good with { Url = server.Url }, folder, null, default, allowLoopbackHttp: true));
            Check(error is UpdateException && server.Requests == 1 && Directory.GetFiles(folder).Length == 0, "a file of another size than the release says is refused at once, nothing left");
        }

        folder = Dir("download-short");
        using (var server = new Server())
        {
            server.Handler = (ctx, n) => Serve(ctx, data, sendOnly: data.Length / 2);
            var error = await Catch(() => SelfUpdate.DownloadAsync(good with { Url = server.Url }, folder, null, default, allowLoopbackHttp: true, retryDelay: Quick));
            Check(error != null && server.Requests == 3 && Directory.GetFiles(folder).Length == 0, $"a file cut short is tried 3 times, then refused, nothing left ({error?.GetType().Name})");
        }

        folder = Dir("download-retry");
        using (var server = new Server())
        {
            server.Handler = (ctx, n) =>
            {
                if (n == 1) { ctx.Response.StatusCode = 500; ctx.Response.Close(); return Task.CompletedTask; }
                return Serve(ctx, data);
            };
            var path = await SelfUpdate.DownloadAsync(good with { Url = server.Url }, folder, null, default, allowLoopbackHttp: true, retryDelay: Quick);
            Check(File.Exists(path) && server.Requests == 2, "a 500 answer is retried and the second try succeeds");
        }

        folder = Dir("download-stall");
        using (var server = new Server())
        {
            server.Handler = (ctx, n) => Serve(ctx, data, sendOnly: 100000, stallAfter: true);
            var watch = Stopwatch.StartNew();
            var error = await Catch(() => SelfUpdate.DownloadAsync(good with { Url = server.Url }, folder, null, default, allowLoopbackHttp: true, stall: TimeSpan.FromMilliseconds(500), retryDelay: Quick));
            Check(error != null && server.Requests == 3 && Directory.GetFiles(folder).Length == 0, $"a stalled download is given up after the stall limit, 3 tries, nothing left ({watch.Elapsed.TotalSeconds:N0} s)");
        }

        folder = Dir("download-cancel");
        using (var server = new Server())
        {
            server.Handler = (ctx, n) => Serve(ctx, data, sendOnly: 100000, stallAfter: true);
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(1.5));
            var error = await Catch(() => SelfUpdate.DownloadAsync(good with { Url = server.Url }, folder, null, cts.Token, allowLoopbackHttp: true));
            Check(error is OperationCanceledException && Directory.GetFiles(folder).Length == 0, "cancelling stops it and leaves no .part");
        }

        folder = Dir("download-refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Url = "http://example.com/MCLA-Setup.exe" }, folder, null, default)) is UpdateException, "plain http to a real address is refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Url = "http://127.0.0.1:9/MCLA-Setup.exe" }, folder, null, default)) is UpdateException, "plain http to the loopback is refused unless a test allows it");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Url = "file:///C:/Windows/notepad.exe" }, folder, null, default, allowLoopbackHttp: true)) is UpdateException, "a file: address is refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Name = @"..\evil.exe", Url = "https://example.com/x" }, folder, null, default)) is UpdateException, "a file name with a path is refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Sha256 = "abc", Url = "https://example.com/x" }, folder, null, default)) is UpdateException, "a missing or short SHA-256 is refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Size = 0, Url = "https://example.com/x" }, folder, null, default)) is UpdateException, "a missing size is refused");
    }

    static async Task<Exception?> Catch(Func<Task> action)
    {
        try { await action(); return null; } catch (Exception ex) { return ex; }
    }

    // ---- the installer -----------------------------------------------------------------------------------------------------

    static string Run(string exe, string arguments, out int exitCode, int seconds = 120)
    {
        var psi = new ProcessStartInfo(exe, arguments) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        using var p = Process.Start(psi)!;
        var output = new StringBuilder();
        p.OutputDataReceived += (_, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        if (!p.WaitForExit(seconds * 1000)) { p.Kill(true); exitCode = -1; return output + "\n(timed out)"; }
        Thread.Sleep(200); // a program the installer started shares its output pipe: not waited for to the end
        exitCode = p.ExitCode;
        return output.ToString();
    }

    static string Compose(string stub, string name, string version, string fakeLauncher, Action<string>? customize = null)
    {
        var folder = Dir("payload-" + name);
        File.Copy(Path.Combine(Environment.SystemDirectory, fakeLauncher), Path.Combine(folder, "MclaLauncher.exe"));
        File.WriteAllBytes(Path.Combine(folder, "launcher.ico"), [0, 0, 1, 0]);
        Write(folder, "version.txt", version);
        Write(folder, @"sub\data.txt", new string('x', 5000) + version);
        var big = new byte[2_000_000];
        new Random(version.GetHashCode()).NextBytes(big);
        File.WriteAllBytes(Path.Combine(folder, "big.bin"), big);
        customize?.Invoke(folder);
        var exe = Path.Combine(Scratch, $"MCLA-Setup-{name}.exe");
        var output = Run(stub, $"--compose \"{folder}\" \"{exe}\" --stub \"{stub}\" --quality 1 --version {version} --tag v{version}", out var code);
        if (code != 0) throw new InvalidOperationException("compose failed: " + output);
        return exe;
    }

    static void Setup(string stub)
    {
        Section("installer (the real MCLA-Setup.exe, fake launchers)");
        var root = Dir("setup");
        var app = Path.Combine(root, "Programs", "MCLA");
        var lnk = Path.Combine(root, "lnk");
        var common = $"--silent --dir \"{app}\" --shortcut-dir \"{lnk}\"";
        // ping.exe stays up for as long as asked: it is the launcher that "starts". --health alive:N is the test form of
        // "a window appeared and stayed".
        const string stay = "--health alive:2 --launch-args \"-n 9 127.0.0.1\"";

        var v1 = Compose(stub, "v1", "1.0.0", "ping.exe");
        var info = Run(v1, "--info", out _);
        Check(info.Contains("version=1.0.0") && info.Contains("files=5"), "--info reads the payload's header");

        // fresh install
        var watch = Stopwatch.StartNew();
        var output = Run(v1, $"{common} {stay}", out var code);
        Check(code == 0, $"fresh install exits 0 ({watch.Elapsed.TotalSeconds:N1} s){(code == 0 ? "" : "\n" + output)}");
        Check(Read(app, "version.txt") == "1.0.0" && File.Exists(Path.Combine(app, "sub", "data.txt")) && Read(app, "installed.txt").Contains("version=1.0.0"), "the files are in place, installed.txt says 1.0.0");
        var lnks = Directory.Exists(lnk) ? Directory.GetFiles(lnk, "*.lnk", SearchOption.AllDirectories) : [];
        Check(lnks.Length == 2, "a Start menu and a desktop shortcut were made (in the scratch folder)");
        Check(FolderSwap.ProcessesUnder(app).Count > 0, "the launcher was started from the new folder");

        // what the wizard writes next to the exe survives updates
        Write(app, "install.json", "{\"installRoot\":\"D:\\\\MCLA\"}");

        // the update: waits for the old launcher, swaps, starts the new one
        var v2 = Compose(stub, "v2", "2.0.0", "ping.exe");
        var oldPid = FolderSwap.ProcessesUnder(app).First().Id;
        watch.Restart();
        output = Run(v2, $"--update --wait-pid {oldPid} {common} --health alive:1 --launch-args \"-n 3 127.0.0.1\"", out code);
        Check(code == 0, $"update exits 0 ({watch.Elapsed.TotalSeconds:N1} s){(code == 0 ? "" : "\n" + output)}");
        Check(Read(app, "version.txt") == "2.0.0" && Read(app, "installed.txt").Contains("version=2.0.0"), "the new version is in place");
        Check(Read(app, "install.json").Contains("D:"), "install.json was carried over");
        Check(watch.Elapsed.TotalSeconds >= 2, "the update waited for the old launcher to end first");
        WaitFree(app);
        Check(!FolderSwap.Leftovers(app).Any(), "the old folder was deleted");

        // an update whose new launcher ends at once is rolled back and the old one is started again
        var bad = Compose(stub, "bad", "3.0.0", "hostname.exe");
        // --update needs --wait-pid; the id of a process that does not exist counts as "already gone"
        output = Run(bad, $"--update --wait-pid {int.MaxValue / 2} {common} --health alive:2 --launch-args \"-n 9 127.0.0.1\"", out code);
        Check(code == 1, $"a launcher that ends at once makes the update fail with exit 1{(code == 1 ? "" : "\n" + output)}");
        Check(Read(app, "version.txt") == "2.0.0" && Read(app, "install.json").Contains("D:"), "the previous version is back, with its install.json");
        WaitFree(app);
        Check(!FolderSwap.Leftovers(app).Any(), "no .new, .old or .failed folder is left after the way back");

        // a newer installed launcher is not replaced by an older installer unless forced; an update does not ask
        var old = Compose(stub, "old", "1.5.0", "ping.exe");
        Run(old, $"{common} --no-launch", out code);
        Check(code == 2 && Read(app, "version.txt") == "2.0.0", "an older installer declines (exit 2) to replace a newer launcher");
        Run(old, $"{common} --no-launch --force", out code);
        Check(code == 0 && Read(app, "version.txt") == "1.5.0", "--force replaces it");

        // a damaged installer file is found before anything changes
        var damaged = Path.Combine(Scratch, "MCLA-Setup-damaged.exe");
        File.Copy(v2, damaged, true);
        using (var f = new FileStream(damaged, FileMode.Open, FileAccess.ReadWrite))
        {
            f.Position = f.Length - 1_000_000;
            var b = f.ReadByte();
            f.Position--;
            f.WriteByte((byte)(b ^ 0x55));
        }
        output = Run(damaged, $"{common} --no-launch --force", out code);
        Check(code == 1 && Read(app, "version.txt") == "1.5.0" && !FolderSwap.Leftovers(app).Any(), $"a damaged installer fails (exit 1) and leaves the installed launcher and the folder beside it alone");

        // a file held open stops the swap with a clear failure and no change
        using (var held = new FileStream(Path.Combine(app, "big.bin"), FileMode.Open, FileAccess.Read, FileShare.None))
        {
            output = Run(v2, $"{common} --no-launch --force --rename-timeout 2", out code);
        }
        Check(code == 1 && Read(app, "version.txt") == "1.5.0" && !FolderSwap.Leftovers(app).Any(), "a held file: exit 1, nothing changed, no leftovers");

        // an interrupted swap is repaired by the next run
        Directory.Move(app, app + ".old-20260101-000000-9");
        Run(v2, $"{common} --no-launch --force", out code);
        Check(code == 0 && Read(app, "version.txt") == "2.0.0" && !FolderSwap.Leftovers(app).Any(), "after a swap that was killed half way, the next run recovers and installs");

        // a first install whose launcher does not start keeps its files (nothing to go back to) and says so
        var app3 = Path.Combine(root, "Programs3", "MCLA");
        output = Run(bad, $"--silent --dir \"{app3}\" --shortcut-dir \"{Path.Combine(root, "lnk3")}\" --health alive:2", out code);
        Check(code == 1 && Read(app3, "version.txt") == "3.0.0", "a first install whose launcher ends at once: exit 1, the files stay");

        // one setup at a time
        var app4 = Path.Combine(root, "Programs4", "MCLA");
        var firstRun = Process.Start(new ProcessStartInfo(v1, $"--silent --dir \"{app4}\" --shortcut-dir \"{Path.Combine(root, "lnk4")}\" --health alive:4 --launch-args \"-n 6 127.0.0.1\"") { UseShellExecute = false, CreateNoWindow = true })!;
        Thread.Sleep(1500);
        var watch2 = Stopwatch.StartNew();
        Run(v1, $"--silent --dir \"{app4}\" --no-launch --no-shortcuts", out code);
        Check(code == 2 && watch2.Elapsed.TotalSeconds < 3, $"a second setup while one runs is turned away at once (exit 2, {watch2.Elapsed.TotalSeconds:N1} s)");
        firstRun.WaitForExit(30000);
        Check(firstRun.ExitCode == 0 && Read(app4, "version.txt") == "1.0.0", "the first setup finished normally");
        WaitFree(app4);

        // the ready-file protocol: the launcher is given a path (MCLA_READY_FILE) and writes it when it has run well
        // (cmd.exe stands in: it writes the file, or just stays up); no file in time, no good
        var app5 = Path.Combine(root, "Programs5", "MCLA");
        var readyFile = Path.Combine(root, "ready.txt");
        var withReady = $"--silent --dir \"{app5}\" --shortcut-dir \"{Path.Combine(root, "lnk5")}\" --ready-file \"{readyFile}\" --health-timeout 4";
        var writes = Compose(stub, "ready", "5.0.0", "cmd.exe");
        output = Run(writes, $"{withReady} --launch-args \"/c echo ready>%MCLA_READY_FILE% & ping -n 4 127.0.0.1 >nul\"", out code);
        Check(code == 0 && File.Exists(readyFile) && Read(app5, "version.txt") == "5.0.0", $"a launcher that writes the ready file passes the start check{(code == 0 ? "" : "\n" + output)}");
        var silent = Compose(stub, "silent", "6.0.0", "cmd.exe");
        output = Run(silent, $"--update --wait-pid {int.MaxValue / 2} {withReady} --launch-args \"/c ping -n 8 127.0.0.1 >nul\"", out code);
        Check(code == 1 && Read(app5, "version.txt") == "5.0.0" && !FolderSwap.Leftovers(app5).Any(), "a launcher that stays up but never writes the ready file is rolled back (the old version is back)");
        Thread.Sleep(1500);

        // a pointer to import: used when the new folder has none, never over one it has
        var pointer = Path.Combine(root, "old-install.json");
        File.WriteAllText(pointer, "{\"installRoot\":\"E:\\\\MCLA-elsewhere\"}");
        var app6 = Path.Combine(root, "Programs6", "MCLA");
        Run(v1, $"--silent --dir \"{app6}\" --no-launch --no-shortcuts --import-pointer \"{pointer}\"", out code);
        Check(code == 0 && Read(app6, "install.json").Contains("elsewhere"), "--import-pointer gives a new install the pointer of the install the launcher had");
        Write(app6, "install.json", "{\"installRoot\":\"D:\\\\mine\"}");
        Run(v2, $"--silent --dir \"{app6}\" --no-launch --no-shortcuts --import-pointer \"{pointer}\"", out code);
        Check(code == 0 && Read(app6, "install.json").Contains("mine") && !Read(app6, "install.json").Contains("elsewhere"), "--import-pointer does not replace the pointer the folder has");

        // refusals: another program's folder, and a fork checkout
        var foreign = Dir("foreign");
        Write(foreign, "notes.txt", "mine");
        output = Run(v1, $"--silent --dir \"{foreign}\" --no-launch --no-shortcuts", out code);
        Check(code == 1 && Read(foreign, "notes.txt") == "mine" && !File.Exists(Path.Combine(foreign, "MclaLauncher.exe")), "a folder that is not the launcher's is refused and left as it was");
        var checkout = Dir("checkout");
        Write(checkout, "build-game.ps1", "# a fork checkout");
        output = Run(v1, $"--silent --dir \"{Path.Combine(checkout, "app")}\" --no-launch --no-shortcuts", out code);
        Check(code == 1 && !Directory.Exists(Path.Combine(checkout, "app")), "a folder inside a fork checkout is refused (the launcher built from source is not touched)");

        // shortcuts that start something else are left alone
        var other = Dir("other-lnk");
        var lnk2 = Path.Combine(other, "desktop", "Midnight Club Los Angeles.lnk");
        Directory.CreateDirectory(Path.GetDirectoryName(lnk2)!);
        MakeLink(lnk2, Path.Combine(Environment.SystemDirectory, "notepad.exe"));
        var app2 = Path.Combine(root, "Programs2", "MCLA");
        Run(v1, $"--silent --dir \"{app2}\" --shortcut-dir \"{other}\" --no-launch", out code);
        Check(code == 0 && LinkTarget(lnk2).EndsWith("notepad.exe", StringComparison.OrdinalIgnoreCase), "a shortcut of the same name that starts another program is not replaced");
        Check(File.Exists(Path.Combine(other, "start-menu", "Midnight Club Los Angeles.lnk")) && LinkTarget(Path.Combine(other, "start-menu", "Midnight Club Los Angeles.lnk")).StartsWith(app2, StringComparison.OrdinalIgnoreCase), "the other shortcut was made");

        // uninstall removes the folder and its own shortcuts only
        Run(v1, $"--uninstall --silent --dir \"{app2}\" --shortcut-dir \"{other}\"", out code);
        Check(code == 0 && !Directory.Exists(app2) && !File.Exists(Path.Combine(other, "start-menu", "Midnight Club Los Angeles.lnk")) && File.Exists(lnk2) && !FolderSwap.Leftovers(app2).Any(), "uninstall removes the folder and its shortcut, not the foreign one");
        Run(v1, $"--uninstall --silent --dir \"{foreign}\"", out code);
        Check(code == 1 && Read(foreign, "notes.txt") == "mine", "uninstall refuses a folder that is not the launcher's");
        WaitFree(app);
    }

    static void WaitFree(string folder)
    {
        var w = Stopwatch.StartNew();
        while (FolderSwap.ProcessesUnder(folder).Count > 0 && w.Elapsed.TotalSeconds < 30) Thread.Sleep(300);
        Thread.Sleep(300);
    }

    static void MakeLink(string lnk, string target)
    {
        var script = $"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{lnk}'); $s.TargetPath='{target}'; $s.Save()";
        Run("powershell.exe", $"-NoProfile -Command \"{script}\"", out _);
    }

    static string LinkTarget(string lnk)
    {
        var script = $"(New-Object -ComObject WScript.Shell).CreateShortcut('{lnk}').TargetPath";
        return Run("powershell.exe", $"-NoProfile -Command \"{script}\"", out _).Trim();
    }
}

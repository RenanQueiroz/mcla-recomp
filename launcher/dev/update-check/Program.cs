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
    /// A junction (no rights needed): link -> target.
    static void MakeJunction(string link, string target)
    {
        var info = new ProcessStartInfo("cmd.exe", $"/c mklink /J \"{link}\" \"{target}\"") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true };
        using var process = Process.Start(info)!;
        process.StandardOutput.ReadToEnd();
        process.WaitForExit();
    }

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

        // a launcher folder is Setup's when it holds installed.txt; the staged folders of the tests are made like Setup makes them
        string Stage(string version, string? extraName = null)
        {
            var staged = FolderSwap.CreateStaging(target);
            Write(staged, "version.txt", version);
            Write(staged, "installed.txt", "version=" + version);
            if (extraName != null) Write(staged, extraName, version);
            return staged;
        }

        // a first install is one rename
        var staged = Stage("1");
        var s1 = FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(2));
        Check(Read(target, "version.txt") == "1" && s1.Previous == null && !Directory.Exists(staged), "first install: the staged folder is the target, nothing before it");
        Check(!File.Exists(Path.Combine(target, FolderSwap.StagingMarker)), "the staging marker does not go into the installed folder");
        FolderSwap.Commit(s1);

        // a replacement keeps what the user's setup wrote (the pointer), and commit removes the old folder
        Write(target, "install.json", "{\"installRoot\":\"C:\\\\MCLA\"}");
        Write(target, "app.dll", "old");
        staged = Stage("2");
        Write(staged, "app.dll", "new");
        FolderSwap.CopyKept(target, staged, ["install.json", "missing.json", @"..\escape.txt", @"C:\Windows\win.ini"]);
        var s2 = FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(2));
        Check(Read(target, "version.txt") == "2" && Read(target, "app.dll") == "new", "replace: the new content is in place");
        Check(Read(target, "install.json").Contains("installRoot"), "replace: install.json was carried over");
        Check(s2.Previous != null && Read(s2.Previous, "version.txt") == "1", "replace: the old folder is beside it until commit");
        Check(!File.Exists(Path.Combine(root, "escape.txt")) && !File.Exists(Path.Combine(target, "win.ini")), "carry-over refuses '..' and rooted names");
        Check(FolderSwap.Leftovers(target).Count() == 1, "replace: the old folder is a leftover Setup knows (exact stamp, installed.txt inside)");
        FolderSwap.Commit(s2);
        Check(!FolderSwap.Leftovers(target).Any(), "commit removes the old folder");

        // the way back
        staged = Stage("3");
        var s3 = FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(2));
        var restored = FolderSwap.RollBack(s3);
        Check(restored && Read(target, "version.txt") == "2", "rollback: version 2 is back");
        Check(!FolderSwap.Leftovers(target).Any(), "rollback: no .failed or .old folder is left");

        // the way back with the old folder gone (something deleted it): nothing is moved, the player keeps a folder (H1)
        staged = Stage("3b");
        var s3b = FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(2));
        FolderSwap.DeleteFolder(s3b.Previous!, TimeSpan.FromSeconds(2));
        var gone = Catch(() => FolderSwap.RollBack(s3b));
        Check(gone is SwapFailedException { OldFolderIsBack: false } && Read(target, "version.txt") == "3b" && !FolderSwap.Leftovers(target).Any(d => d.Contains(FolderSwap.FailedTag)),
              "rollback with the previous folder gone: refused, the new folder stays where it is");
        staged = Stage("2again");
        FolderSwap.Commit(FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(2)));

        // a held file stops the swap before anything changes
        staged = Stage("4");
        using (var held = new FileStream(Path.Combine(target, "version.txt"), FileMode.Open, FileAccess.Read, FileShare.None))
        {
            var watch = Stopwatch.StartNew();
            Exception? error = null;
            try { FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(1.5)); } catch (Exception ex) { error = ex; }
            Check(error is IOException && error is not SwapFailedException && watch.Elapsed.TotalSeconds >= 1.4, $"locked: gives up after the timeout ({watch.Elapsed.TotalSeconds:N1} s) with an IOException");
            Check(Directory.Exists(target) && File.Exists(Path.Combine(target, "version.txt")) && !FolderSwap.Leftovers(target, FolderSwap.OldTag).Any(), "locked: the installed folder is where it was, with no .old beside it");
        }
        Check(Directory.Exists(staged), "locked: the staged folder is still there for a retry");
        FolderSwap.DeleteFolder(staged, TimeSpan.FromSeconds(2));

        // the second rename fails (the staged folder is held): the old folder is put back, the staged one is left for the caller (M5)
        staged = Stage("5");
        var oldVersion = File.ReadAllText(Path.Combine(target, "version.txt")).Trim();
        using (var heldStaged = new FileStream(Path.Combine(staged, "version.txt"), FileMode.Open, FileAccess.Read, FileShare.None))
        {
            var error = Catch(() => FolderSwap.Apply(target, staged, TimeSpan.FromSeconds(1)));
            Check(error is SwapFailedException { OldFolderIsBack: true } && Directory.Exists(target) && Read(target, "installed.txt").Contains(oldVersion) && Directory.Exists(staged),
                  "a staged folder that can't be moved in: the old folder is put back, the staged one stays");
        }
        Check(!FolderSwap.Leftovers(target, FolderSwap.OldTag).Any() && FolderSwap.Leftovers(target, FolderSwap.NewTag).Count() == 1, "... and what is beside it is the staged folder, with its marker (kept through the renames)");
        FolderSwap.DeleteFolder(staged, TimeSpan.FromSeconds(2));

        // killed between the two renames: Recover puts the old folder back (only one that is Setup's)
        var crashed = target + ".old-20260101-000000-1";
        Directory.Move(target, crashed);
        Check(!Directory.Exists(target), "setup of the crash case: no target");
        Check(FolderSwap.Recover(target) && File.Exists(Path.Combine(target, "installed.txt")), "recover: the previous folder came back");
        Check(!FolderSwap.Recover(target), "recover: nothing to do when the folder is there");
        Directory.Move(target, target + "-moved");
        Directory.CreateDirectory(target + ".old-backup");                      // a player's folder with a similar name
        Write(target + ".old-20260101-000000-2", "notes.txt", "mine");          // an exact stamp, but no installed.txt: not Setup's
        Check(!FolderSwap.Recover(target) && !Directory.Exists(target), "recover: a similar name, or an exact stamp without Setup's marker, is not restored as the launcher");
        Directory.Move(target + "-moved", target);
        FolderSwap.DeleteFolder(target + ".old-backup", TimeSpan.FromSeconds(2));
        FolderSwap.DeleteFolder(target + ".old-20260101-000000-2", TimeSpan.FromSeconds(2));

        // clean-up removes only the swap's leftovers: exact stamp, Setup's marker inside
        Write(target + ".old-20261001-120000-11", "installed.txt", "x");
        Write(target + ".failed-20261001-120000-12", "installed.txt", "x");
        Write(target + ".new-20261001-120000-13", FolderSwap.StagingMarker, "x");
        Write(target + ".old-20261001-120000-14", "notes.txt", "a player's folder, an exact stamp");
        Write(target + ".old-backup", "installed.txt", "x");                       // a similar name
        Write(target + ".new-1", FolderSwap.StagingMarker, "x");                    // not a stamp
        Directory.CreateDirectory(target + ".keep");
        Directory.CreateDirectory(target + "2");
        var removed = FolderSwap.CleanLeftovers(target);
        Check(removed == 3 && Directory.Exists(target + ".old-20261001-120000-14") && Directory.Exists(target + ".old-backup") && Directory.Exists(target + ".new-1")
              && Directory.Exists(target + ".keep") && Directory.Exists(target + "2") && Directory.Exists(target), "clean-up: removed the 3 leftovers that are Setup's and nothing else");
        foreach (var other in new[] { ".old-20261001-120000-14", ".old-backup", ".new-1", ".keep" }) FolderSwap.DeleteFolder(target + other, TimeSpan.FromSeconds(2));
        FolderSwap.DeleteFolder(target + "2", TimeSpan.FromSeconds(2));

        // what a launcher folder holds
        Write(target, "install.json", "{\"installRoot\":\"" + Path.Combine(target, "game").Replace("\\", "\\\\") + "\"}");
        Check(FolderSwap.InstallRootInside(target)?.EndsWith("game") == true, "an install.json that points inside the launcher's folder is found");
        Write(target, "install.json", "{\"installRoot\":\"" + (target + "2").Replace("\\", "\\\\") + "\"}");
        Check(FolderSwap.InstallRootInside(target) == null, "an install.json that points elsewhere (even to a folder with the same start) is not");
        Write(target, "install.json", "not json at all");
        Check(FolderSwap.InstallRootInside(target) == null, "an install.json that is not JSON is not a reason to refuse");
        File.Delete(Path.Combine(target, "install.json"));
        Write(target, "notes.txt", "a player's file");
        Directory.CreateDirectory(Path.Combine(target, "game"));
        var owned = FolderSwap.TopLevelNames(["version.txt", "sub/data.bin"]);
        var foreign = FolderSwap.ForeignEntries(target, owned);
        Check(foreign.SequenceEqual(["game", "notes.txt"]), $"foreign entries are the names the payload does not have: {string.Join(", ", foreign)}");
        Write(target, "sub/data.bin", "x");
        Write(target, "installed-files.txt", "version.txt\nsub/data.bin\n");
        // at any depth: a file of the player's in a folder the payload has, an empty folder of theirs, a link; a stray staging marker is not one
        Write(target, "sub/mine.txt", "the player's");
        Directory.CreateDirectory(Path.Combine(target, "emptyfolder"));
        Write(target, ".mcla-staging", "left by a kill");
        var deep = FolderSwap.ForeignFiles(target, FolderSwap.ReadManifest(target)!, ["install.json"]);
        Check(deep.SequenceEqual(["emptyfolder/", "game/", "notes.txt", "sub/mine.txt"]), $"foreign files are looked for at any depth, owned folders included: {string.Join(", ", deep)}");
        // a link inside a folder of the payload is one entry and is never followed; Explorer's own files are not foreign; the list is capped
        var outsideTarget = Dir("their-folder");
        Write(outsideTarget, "theirs.txt", "the player's, behind a junction");
        var linkInside = Path.Combine(target, "sub", "link");
        MakeJunction(linkInside, outsideTarget);
        Write(target, "desktop.ini", "[.ShellClassInfo]");
        Write(target, "sub/Thumbs.db", "x");
        var withLink = FolderSwap.ForeignFiles(target, FolderSwap.ReadManifest(target)!, ["install.json"]);
        Check(withLink.SequenceEqual(["emptyfolder/", "game/", "notes.txt", "sub/link/", "sub/mine.txt"]),
              $"a link inside a payload folder is one entry, desktop.ini and Thumbs.db are not foreign: {string.Join(", ", withLink)}");
        Directory.CreateDirectory(Path.Combine(target, "sub", "many"));
        for (var i = 0; i < 300; i++) Write(target, $"sub/f{i:000}.txt", "x");
        var capped = FolderSwap.ForeignFiles(target, FolderSwap.ReadManifest(target)!, ["install.json"]);
        Check(capped.Count == 200, $"the walk stops at 200 entries: {capped.Count}");
        Check(FolderSwap.ForeignFiles(target, FolderSwap.ReadManifest(target)!, ["install.json"], limit: 7).Count == 7, "... or at the limit given");
        for (var i = 0; i < 300; i++) File.Delete(Path.Combine(target, "sub", $"f{i:000}.txt"));
        Directory.Delete(Path.Combine(target, "sub", "many"));
        Directory.Delete(linkInside);                 // the junction only
        Check(File.Exists(Path.Combine(outsideTarget, "theirs.txt")), "removing the junction left what it pointed to");
        File.Delete(Path.Combine(target, "desktop.ini"));
        File.Delete(Path.Combine(target, "sub", "Thumbs.db"));

        // what the launcher itself would say before it quits to hand over
        var why = SelfUpdate.WhyNotUpdatable(target);
        Check(why != null && why.Contains("sub/mine.txt"), "the launcher names what would stop an update, before it quits");
        File.Delete(Path.Combine(target, "sub/mine.txt"));
        Directory.Delete(Path.Combine(target, "emptyfolder"));
        File.Delete(Path.Combine(target, ".mcla-staging"));
        Directory.Delete(Path.Combine(target, "game"));
        File.Delete(Path.Combine(target, "notes.txt"));
        Check(SelfUpdate.WhyNotUpdatable(target) == null, "... and says nothing when the folder is only Setup's");
        Write(target, "install.json", "{\"installRoot\":\"" + Path.Combine(target, "game").Replace("\\", "\\\\") + "\"}");
        Check(SelfUpdate.WhyNotUpdatable(target)?.Contains("inside the launcher's folder") == true, "... or an install inside the launcher's folder");
        File.Delete(Path.Combine(target, "install.json"));
        Directory.CreateDirectory(Path.Combine(target, "game"));
        Write(target, "notes.txt", "a player's file");
        var left = FolderSwap.DeleteOwned(target, FolderSwap.ReadManifest(target)!, TimeSpan.FromSeconds(2));
        Check(left.SequenceEqual(["game", "notes.txt"]) && Directory.Exists(target) && !Directory.Exists(Path.Combine(target, "sub")) && !File.Exists(Path.Combine(target, "version.txt")),
              "removing the launcher takes only the files Setup unpacked (and the folders then empty); the player's entries and the folder stay");
        File.Delete(Path.Combine(target, "notes.txt"));
        Directory.Delete(Path.Combine(target, "game"));
        Write(target, "version.txt", "2");
        Write(target, "installed.txt", "version=2");

        // removing the launcher does not follow a link on the way to a listed file (the player replaced a payload folder by a junction to
        // a folder of their own), and takes Explorer's files away with the rest
        var removal = Dir("removal");
        Write(removal, "lib/a.dll", "ours");
        Write(removal, "own.txt", "ours");
        Write(removal, "desktop.ini", "explorer");
        var theirs = Dir("their-lib");
        Write(theirs, "a.dll", "the player's file with a name of ours");
        FolderSwap.DeleteFolder(Path.Combine(removal, "lib"), TimeSpan.FromSeconds(2));
        MakeJunction(Path.Combine(removal, "lib"), theirs);
        var rest = FolderSwap.DeleteOwned(removal, ["lib/a.dll", "own.txt"], TimeSpan.FromSeconds(2));
        Check(File.Exists(Path.Combine(theirs, "a.dll")) && Directory.Exists(Path.Combine(removal, "lib")) && !File.Exists(Path.Combine(removal, "own.txt"))
              && !File.Exists(Path.Combine(removal, "desktop.ini")) && rest.SequenceEqual(["lib"]),
              $"uninstall: a listed file behind a link is not deleted, the link stays, Explorer's file goes: {string.Join(", ", rest)}");
        Directory.Delete(Path.Combine(removal, "lib"));
        Directory.Delete(removal);

        // the launcher's clean-up waits while a Setup runs (its mutex exists): the previous folder is for the way back (H1)
        Write(target + ".old-20261001-120000-21", "installed.txt", "x");
        bool whileRunning, afterwards;
        using (new Mutex(true, @"Local\MCLA-Setup"))
        {
            Check(SelfUpdate.SetupIsRunning(), "a running Setup is seen (its mutex)");
            whileRunning = SelfUpdate.CleanUp(target);
            Check(!whileRunning && Directory.Exists(target + ".old-20261001-120000-21"), "the launcher's clean-up does nothing while a Setup is running");
        }
        afterwards = SelfUpdate.CleanUp(target);
        Check(afterwards && !Directory.Exists(target + ".old-20261001-120000-21"), "... and removes the leftovers once it is not");
        var downloads = SelfUpdate.DownloadFolder(target);
        Write(downloads, "MCLA-Setup.exe", "somebody's file");
        SelfUpdate.CleanUp(target);
        Check(File.Exists(Path.Combine(downloads, "MCLA-Setup.exe")), "a download folder without Setup's marker is not removed");
        File.WriteAllText(Path.Combine(downloads, FolderSwap.DownloadMarker), "x");
        SelfUpdate.CleanUp(target);
        Check(!Directory.Exists(downloads), "one with the marker is");

        // a link is seen
        var link = Path.Combine(root, "link");
        Run("cmd.exe", $"/c mklink /J \"{link}\" \"{target}\"", out _);
        Check(Directory.Exists(link) && FolderSwap.IsLink(link) && !FolderSwap.IsLink(target), "a junction is recognised as a link");
        Directory.Delete(link);

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

    static Exception? Catch(Action action)
    {
        try { action(); return null; } catch (Exception ex) { return ex; }
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
            Check(NothingLeft(folder), "nothing is left behind after a wrong SHA-256");
        }

        folder = Dir("download-size");
        using (var server = new Server())
        {
            var longer = data.Concat(new byte[10]).ToArray();
            server.Handler = (ctx, n) => Serve(ctx, longer);
            var error = await Catch(() => SelfUpdate.DownloadAsync(good with { Url = server.Url }, folder, null, default, allowLoopbackHttp: true));
            Check(error is UpdateException && server.Requests == 1 && NothingLeft(folder), "a file of another size than the release says is refused at once, nothing left");
        }

        folder = Dir("download-short");
        using (var server = new Server())
        {
            server.Handler = (ctx, n) => Serve(ctx, data, sendOnly: data.Length / 2);
            var error = await Catch(() => SelfUpdate.DownloadAsync(good with { Url = server.Url }, folder, null, default, allowLoopbackHttp: true, retryDelay: Quick));
            Check(error != null && server.Requests == 3 && NothingLeft(folder), $"a file cut short is tried 3 times, then refused, nothing left ({error?.GetType().Name})");
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
            Check(error != null && server.Requests == 3 && NothingLeft(folder), $"a stalled download is given up after the stall limit, 3 tries, nothing left ({watch.Elapsed.TotalSeconds:N0} s)");
        }

        folder = Dir("download-cancel");
        using (var server = new Server())
        {
            server.Handler = (ctx, n) => Serve(ctx, data, sendOnly: 100000, stallAfter: true);
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(1.5));
            var error = await Catch(() => SelfUpdate.DownloadAsync(good with { Url = server.Url }, folder, null, cts.Token, allowLoopbackHttp: true));
            Check(error is OperationCanceledException && NothingLeft(folder), "cancelling stops it and leaves no .part");
        }

        folder = Dir("download-refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Url = "http://example.com/MCLA-Setup.exe" }, folder, null, default)) is UpdateException, "plain http to a real address is refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Url = "http://127.0.0.1:9/MCLA-Setup.exe" }, folder, null, default)) is UpdateException, "plain http to the loopback is refused unless a test allows it");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Url = "file:///C:/Windows/notepad.exe" }, folder, null, default, allowLoopbackHttp: true)) is UpdateException, "a file: address is refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Name = @"..\evil.exe", Url = "https://example.com/x" }, folder, null, default)) is UpdateException, "a file name with a path is refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Sha256 = "abc", Url = "https://example.com/x" }, folder, null, default)) is UpdateException, "a missing or short SHA-256 is refused");
        Check(await Catch(() => SelfUpdate.DownloadAsync(good with { Size = 0, Url = "https://example.com/x" }, folder, null, default)) is UpdateException, "a missing size is refused");

        // a development release source is a folder: the file is copied and checked the same way
        folder = Dir("download-copy");
        var local = Path.Combine(Scratch, "release-folder");
        Directory.CreateDirectory(local);
        File.WriteAllBytes(Path.Combine(local, "MCLA-Setup.exe"), data);
        var copied = await SelfUpdate.CopyVerifiedAsync(Path.Combine(local, "MCLA-Setup.exe"), good, folder, default);
        Check(File.Exists(copied) && File.ReadAllBytes(copied).AsSpan().SequenceEqual(data) && !File.Exists(copied + ".part"), "a file from a local release source is copied and checked");
        var wrongData = (byte[])data.Clone();
        wrongData[5] ^= 1;
        File.WriteAllBytes(Path.Combine(local, "wrong.exe"), wrongData);
        folder = Dir("download-copy-bad");
        Check(await Catch(() => SelfUpdate.CopyVerifiedAsync(Path.Combine(local, "wrong.exe"), good, folder, default)) is UpdateException && NothingLeft(folder), "a copy with the wrong SHA-256 is refused and nothing is left");
        Check(await Catch(() => SelfUpdate.CopyVerifiedAsync(Path.Combine(local, "missing.exe"), good, folder, default)) is UpdateException, "a file missing from the local release source is refused");
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
        var deletedLnk = Directory.GetFiles(lnk, "*.lnk", SearchOption.AllDirectories).OrderBy(f => f).First(); // the player deleted one
        File.Delete(deletedLnk);
        watch.Restart();
        output = Run(v2, $"--update --wait-pid {oldPid} {common} --health alive:1 --launch-args \"-n 3 127.0.0.1\"", out code);
        Check(code == 0, $"update exits 0 ({watch.Elapsed.TotalSeconds:N1} s){(code == 0 ? "" : "\n" + output)}");
        Check(Read(app, "version.txt") == "2.0.0" && Read(app, "installed.txt").Contains("version=2.0.0"), "the new version is in place");
        Check(Read(app, "install.json").Contains("D:"), "install.json was carried over");
        Check(!File.Exists(deletedLnk) && Directory.GetFiles(lnk, "*.lnk", SearchOption.AllDirectories).Length == 1, "an update renews the shortcuts that are there and does not make a deleted one again");
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
        var withReady = $"--silent --dir \"{app5}\" --shortcut-dir \"{Path.Combine(root, "lnk5")}\" --ready-file \"{readyFile}\" --health alive:1 --health-timeout 4";
        var writes = Compose(stub, "ready", "5.0.0", "cmd.exe");
        output = Run(writes, $"{withReady} --launch-args \"/c echo ready>%MCLA_READY_FILE% & ping -n 4 127.0.0.1 >nul\"", out code);
        Check(code == 0 && !File.Exists(readyFile) && Read(app5, "version.txt") == "5.0.0", $"a launcher that writes the ready file passes the start check (and the installer removes the file){(code == 0 ? "" : "\n" + output)}");
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

        // ---- what the installer may replace or remove (the review's H2, M1, L1, L3) ----------------------------------------------
        // a folder that has the launcher's program but was not made by Setup (no installed.txt): the unpacked zip next to the player's files
        var zipFolder = Dir("zip-next-to-files");
        File.Copy(Path.Combine(Environment.SystemDirectory, "ping.exe"), Path.Combine(zipFolder, "MclaLauncher.exe"));
        Write(zipFolder, "disc-image-notes.txt", "mine");
        var before = Directory.GetFiles(zipFolder).Length;
        Run(v1, $"--silent --dir \"{zipFolder}\" --no-launch --no-shortcuts", out code);
        var installRefused = code;
        Run(v2, $"--update --wait-pid {int.MaxValue / 2} --silent --dir \"{zipFolder}\" --no-launch --no-shortcuts", out code);
        var updateRefused = code;
        Run(v1, $"--uninstall --silent --dir \"{zipFolder}\"", out code);
        Check(installRefused == 1 && updateRefused == 1 && code == 1 && Directory.GetFiles(zipFolder).Length == before && Read(zipFolder, "disc-image-notes.txt") == "mine",
              "a folder with MclaLauncher.exe and other files but no installed.txt is refused by install, update and uninstall, and left as it was");

        // an install the player put inside the launcher's folder (and anything else the payload does not have) is never deleted
        var app7 = Path.Combine(root, "Programs7", "MCLA");
        Run(v1, $"--silent --dir \"{app7}\" --no-launch --no-shortcuts", out code);
        Check(code == 0 && File.Exists(Path.Combine(app7, "installed-files.txt")) && Read(app7, "installed-files.txt").Contains("version.txt"), "an install writes the list of the files it unpacked (installed-files.txt)");
        Write(app7, @"game\default.xex", "the player's game");
        Run(v2, $"--update --wait-pid {int.MaxValue / 2} --silent --dir \"{app7}\" --no-launch --no-shortcuts", out code);
        Check(code == 1 && Read(app7, "version.txt") == "1.0.0" && Read(app7, @"game\default.xex") == "the player's game" && !FolderSwap.Leftovers(app7).Any(),
              "an update of a folder that holds something the setup did not unpack is refused, and nothing is touched");
        Run(v1, $"--uninstall --silent --dir \"{app7}\"", out code);
        Check(code == 0 && Read(app7, @"game\default.xex") == "the player's game" && !File.Exists(Path.Combine(app7, "MclaLauncher.exe")) && !File.Exists(Path.Combine(app7, "installed.txt")),
              "uninstall removes only the files the setup unpacked: the player's game stays, and so does its folder");
        var app8 = Path.Combine(root, "Programs8", "MCLA");
        Run(v1, $"--silent --dir \"{app8}\" --no-launch --no-shortcuts", out code);
        Write(app8, "install.json", "{\"installRoot\":\"" + Path.Combine(app8, "game").Replace("\\", "\\\\") + "\",\"state\":\"ready\"}");
        Write(app8, @"game\default.xex", "the player's game");
        Run(v2, $"--update --wait-pid {int.MaxValue / 2} --silent --dir \"{app8}\" --no-shortcuts --launch-args \"-n 5 127.0.0.1\"", out code);
        var rootRefusedUpdate = code;
        Thread.Sleep(700);
        Check(FolderSwap.ProcessesUnder(app8).Count > 0, "a refused update (an install inside the folder) leaves the launcher running: it is started again");
        WaitFree(app8);
        Run(v1, $"--uninstall --silent --dir \"{app8}\"", out code);
        Check(rootRefusedUpdate == 1 && code == 1 && Read(app8, @"game\default.xex") == "the player's game" && File.Exists(Path.Combine(app8, "MclaLauncher.exe")),
              "an update or an uninstall of a folder that holds the install its install.json names is refused, nothing touched");

        // a player's file inside a folder that the payload also has is foreign too (it would go with the old folder)
        var app12 = Path.Combine(root, "Programs12", "MCLA");
        Run(v1, $"--silent --dir \"{app12}\" --no-launch --no-shortcuts", out code);
        Write(app12, @"sub\mine.txt", "the player's file");
        Run(v2, $"--update --wait-pid {int.MaxValue / 2} --silent --dir \"{app12}\" --no-launch --no-shortcuts", out code);
        Check(code == 1 && Read(app12, @"sub\mine.txt") == "the player's file" && Read(app12, "version.txt") == "1.0.0", "a file of the player's inside a folder of the payload also stops an update, and is kept");

        // a junction as --dir, a drive's root, the user's own folder, and --update with nothing there
        var linked = Path.Combine(root, "linked");
        Run("cmd.exe", $"/c mklink /J \"{linked}\" \"{app7}\"", out _);
        var linkFilesBefore = Directory.GetFileSystemEntries(app7).Length;
        Run(v1, $"--silent --dir \"{linked}\" --no-launch --no-shortcuts", out code);
        Check(code == 1 && Directory.GetFileSystemEntries(app7).Length == linkFilesBefore, "a junction as --dir is refused and what it points at is not touched");
        if (Directory.Exists(linked)) Directory.Delete(linked);
        var driveRoot = Path.GetPathRoot(root)!;
        var rootArgument = driveRoot + @"\"; // two backslashes before the closing quote make one, so the quote is not swallowed
        Run(v1, $"--silent --dir \"{rootArgument}\" --no-launch --no-shortcuts", out code);
        Check(code == 1, $"a drive's root ({driveRoot}) is refused");
        Run(v1, $"--silent --dir \"{Environment.GetFolderPath(Environment.SpecialFolder.UserProfile)}\" --no-launch --no-shortcuts", out code);
        Check(code == 1, "the user's profile folder is refused");
        Run(v2, $"--update --wait-pid {int.MaxValue / 2} --silent --dir \"{Path.Combine(root, "nothing-here")}\" --no-launch --no-shortcuts", out code);
        Check(code == 1 && !Directory.Exists(Path.Combine(root, "nothing-here")), "--update with no launcher in the folder fails and makes nothing");

        // an update that fails before the folders change leaves the old launcher running (the person had nothing open)
        var app9 = Path.Combine(root, "Programs9", "MCLA");
        Run(v1, $"--silent --dir \"{app9}\" --no-launch --no-shortcuts", out code);
        output = Run(damaged, $"--update --wait-pid {int.MaxValue / 2} --silent --dir \"{app9}\" --no-shortcuts --launch-args \"-n 8 127.0.0.1\"", out code);
        Thread.Sleep(700);
        Check(code == 1 && FolderSwap.ProcessesUnder(app9).Count > 0 && Read(app9, "version.txt") == "1.0.0", "a failed update (a damaged installer) starts the old launcher again");
        WaitFree(app9);

        // a payload that holds more bytes than its header says is refused (the header is not covered by the checksums of the files)
        var inflated = Path.Combine(Scratch, "MCLA-Setup-inflated.exe");
        File.Copy(v1, inflated, true);
        var exeBytes = File.ReadAllBytes(inflated);
        var at = IndexOf(exeBytes, Encoding.ASCII.GetBytes("\nbytes="));
        exeBytes[at + 7] = (byte)'1'; // the first digit of the byte count: 2,0xx,xxx becomes 1,0xx,xxx
        exeBytes[at + 8] = (byte)'0';
        File.WriteAllBytes(inflated, exeBytes);
        var app11 = Path.Combine(root, "Programs11", "MCLA");
        output = Run(inflated, $"--silent --dir \"{app11}\" --no-launch --no-shortcuts", out code);
        Check(code == 1 && !Directory.Exists(app11) && !Directory.GetDirectories(Path.GetDirectoryName(app11)!, "MCLA.*").Any(), "a payload with more bytes than its header says is refused, and nothing is left");

        // a shortcut file of the name that cannot be read, or whose program is not there, is not replaced either
        var odd = Dir("odd-lnk");
        Directory.CreateDirectory(Path.Combine(odd, "start-menu"));
        File.WriteAllBytes(Path.Combine(odd, "start-menu", "Midnight Club Los Angeles.lnk"), [1, 2, 3, 4]);
        Directory.CreateDirectory(Path.Combine(odd, "desktop"));
        MakeLink(Path.Combine(odd, "desktop", "Midnight Club Los Angeles.lnk"), @"C:\mcla-no-such-folder\MclaLauncher.exe");
        Run(v1, $"--silent --dir \"{Path.Combine(root, "Programs10", "MCLA")}\" --shortcut-dir \"{odd}\" --no-launch", out code);
        Check(code == 0 && File.ReadAllBytes(Path.Combine(odd, "start-menu", "Midnight Club Los Angeles.lnk")).SequenceEqual(new byte[] { 1, 2, 3, 4 }) && LinkTarget(Path.Combine(odd, "desktop", "Midnight Club Los Angeles.lnk")).Contains("mcla-no-such-folder"),
              "a shortcut that can't be read, or whose program is gone, is left alone");

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

    /// Nothing of a download is left in the folder but Setup's marker of it.
    static bool NothingLeft(string folder) => Directory.GetFiles(folder).All(f => Path.GetFileName(f) == FolderSwap.DownloadMarker);

    static int IndexOf(byte[] data, byte[] pattern)
    {
        for (var i = data.Length - pattern.Length; i >= 0; i--)
            if (data.AsSpan(i, pattern.Length).SequenceEqual(pattern)) return i;
        throw new InvalidOperationException("pattern not found");
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

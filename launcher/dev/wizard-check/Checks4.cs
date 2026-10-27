// Checks of the network side of the setup (fork issue 13): the real GitHub through the new client (release.json, a redirect to
// the asset CDN, a tool archive, GitHub's API route), and "Use files I downloaded" (the folder as a release source, with the
// checksums still applying). The connect logic and the stage messages are in launcher\dev\net-check.
//   net <scratch>                        needs the internet: reads the latest release and downloads two small files
//   offline <release folder> <scratch>   <release folder> holds a release.json (a download of the real one will do); no internet
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using MclaLauncher.Setup;

namespace MclaLauncher;

static partial class Program
{
    static async Task NetChecks(string scratch)
    {
        SafeFolder.Delete(scratch);
        Directory.CreateDirectory(scratch);
        var watch = Stopwatch.StartNew();
        var source = ReleaseSource.FromOverride(null);
        var release = await source.LoadAsync(CancellationToken.None, line => Console.WriteLine($"      the page says: {line}"));
        Check("GitHub's latest release is read, by the direct address", source.Route == ReleaseRoute.Direct, $"{release.Tag} {release.Commit[..8]} in {watch.ElapsedMilliseconds} ms");
        Console.WriteLine(source.LastOperation!.Report());

        // A tool archive of another repository (github.com, then a redirect to the asset CDN), checked against release.json.
        var ninja = release.Tools["ninja"];
        var file = Path.Combine(scratch, "downloads", Path.GetFileName(new Uri(ninja.Url).AbsolutePath));
        watch.Restart();
        await Verified.FetchAsync("ninja", file, ninja.Size, ninja.Sha256, (destination, progress, token) => ReleaseSource.DownloadAsync(ninja.Url, destination, progress, token),
                                  new Progress<long>(), line => Console.WriteLine("      " + line), CancellationToken.None);
        Check("a tool archive comes through the redirect to the CDN and matches release.json", Verified.Is(file, ninja.Size, ninja.Sha256), $"{ninja.Size:N0} bytes in {watch.ElapsedMilliseconds} ms");

        // An asset of the fork's own release.
        var notices = Path.Combine(scratch, "downloads", "NOTICES.txt");
        watch.Restart();
        await ReleaseSource.DownloadAsync(source.AssetUrl("NOTICES.txt"), notices, new Progress<long>(), CancellationToken.None);
        Check("an asset of the fork's release is downloaded", new FileInfo(notices).Length > 1000, $"{new FileInfo(notices).Length:N0} bytes in {watch.ElapsedMilliseconds} ms");

        // The API route, against the real API (one request of the 60 an hour).
        var op = new NetOperation("find the latest release (API route)");
        watch.Restart();
        var viaApi = await ReleaseFetch.ViaApiAsync(source.Endpoints, op, new ReleaseFetch.Timing(), CancellationToken.None);
        Check("GitHub's API route gives the same release.json", ReleaseInfo.Parse(viaApi).Commit == release.Commit, $"{watch.ElapsedMilliseconds} ms");
        Console.WriteLine(op.Report());

        // Through a proxy (a small CONNECT proxy of this program, on 127.0.0.1): the connect callback connects to the proxy, the
        // proxy opens the tunnel to GitHub and then to the asset CDN, and the name of GitHub is not looked up by this program.
        await ProxyCheck(source);

        // A name that does not exist, a real failure: the message names the stage.
        var bad = new NetOperation("nothing");
        try { using var r = await NetClient.GetAsync("https://nonexistent-host.invalid/x", bad, null, CancellationToken.None); Check("an unknown host fails", false); }
        catch (NetworkFailure f) { Check("an unknown host is a DNS lookup failure", f.Stage == NetStage.Lookup, f.Plain); }
    }

    static async Task ProxyCheck(ReleaseSource source)
    {
        var listener = new System.Net.Sockets.TcpListener(System.Net.IPAddress.Loopback, 0);
        listener.Start();
        var tunnels = new System.Collections.Concurrent.ConcurrentQueue<string>();
        _ = Task.Run(async () =>
        {
            while (true)
            {
                System.Net.Sockets.TcpClient client;
                try { client = await listener.AcceptTcpClientAsync(); } catch (Exception) { return; }
                _ = Task.Run(async () =>
                {
                    using var down = client;
                    var stream = down.GetStream();
                    var head = new StringBuilder();
                    var one = new byte[1];
                    while (!head.ToString().EndsWith("\r\n\r\n") && await stream.ReadAsync(one) == 1) head.Append((char)one[0]);
                    var first = head.ToString().Split("\r\n")[0].Split(' ');
                    if (first.Length < 2 || first[0] != "CONNECT") return;
                    tunnels.Enqueue(first[1]);
                    var target = first[1].Split(':');
                    using var up = new System.Net.Sockets.TcpClient();
                    await up.ConnectAsync(target[0], int.Parse(target[1]));
                    var reply = Encoding.ASCII.GetBytes("HTTP/1.1 200 Connection established\r\n\r\n");
                    await stream.WriteAsync(reply);
                    var upstream = up.GetStream();
                    await Task.WhenAny(stream.CopyToAsync(upstream), upstream.CopyToAsync(stream));
                });
            }
        });
        try
        {
            var port = ((System.Net.IPEndPoint)listener.LocalEndpoint).Port;
            using var client = NetClient.Create(NetClient.CreateHandler(new System.Net.WebProxy($"http://127.0.0.1:{port}")));
            var op = new NetOperation("find the latest release (through a proxy)");
            using var response = await NetClient.GetAsync(source.ReleaseJsonUrl, op, new NetRequest { Client = client }, CancellationToken.None);
            var text = await NetClient.ReadTextAsync(response, op, TimeSpan.FromSeconds(10), 1 << 20, CancellationToken.None);
            Check("release.json arrives through the proxy", ReleaseInfo.Parse(text).Repository == ReleaseSource.Repository);
            Check("the proxy was asked for a tunnel to GitHub and to the asset CDN", tunnels.Contains("github.com:443") && tunnels.Any(t => t.StartsWith("release-assets.githubusercontent.com:443")), string.Join(", ", tunnels));
            Check("this program looked up no GitHub name itself", !op.Events.Any(e => e.Contains("DNS github.com") || e.Contains("DNS release-assets")));
        }
        finally { listener.Stop(); }
    }

    static async Task OfflineChecks(string releaseFolder, string scratch)
    {
        SafeFolder.Delete(scratch);
        var folder = Path.Combine(scratch, "my downloads");
        Directory.CreateDirectory(folder);
        var original = File.ReadAllText(Path.Combine(releaseFolder, "release.json"));
        File.WriteAllText(Path.Combine(folder, "release.json"), original);

        var source = ReleaseSource.FromDownloads(folder);
        var release = await source.LoadAsync(CancellationToken.None);
        Check("release.json is read from the player's folder", source.IsOffline && source.IsOverride && release.Tag.Length > 0, release.Tag);
        Check("it is not a development option (no banner)", DevOptions.Current.ReleaseSource == null);

        var files = source.OfflineFiles();
        Check("the SDK bundle and the four tool archives are listed with their addresses",
              files.Count == 5 && files[0].Name == release.SdkBundle!.Asset && files[0].Url.StartsWith("https://github.com/RenanQueiroz/mcla-recomp/releases/download/") && files.Skip(1).All(f => f.Url.StartsWith("https://")),
              string.Join(", ", files.Select(f => f.Name)));
        Check("none is present in an empty folder", files.All(f => !f.Present));

        // A file with the right size and the wrong bytes counts as present (the size is all the page looks at) and is refused when it is used.
        var bundle = release.SdkBundle!;
        var bundleFile = Path.Combine(folder, bundle.Asset);
        using (var fake = new FileStream(bundleFile, FileMode.Create)) fake.SetLength(bundle.Size);
        Check("a file of the right size is listed as present", source.OfflineFiles()[0].Present);
        var downloads = Path.Combine(scratch, "dl1");
        try
        {
            await Verified.FetchAsync("the SDK bundle", Path.Combine(downloads, bundle.Asset), bundle.Size, bundle.Sha256,
                (destination, progress, token) => source.FetchAssetAsync(bundle.Asset, destination, progress, token), new Progress<long>(), _ => { }, CancellationToken.None);
            Check("wrong bytes of the right size are refused by the checksum", false, "accepted");
        }
        catch (SetupException ex) { Check("wrong bytes of the right size are refused by the checksum", ex.Plain.Contains("doesn't match the release's checksum") && !File.Exists(Path.Combine(downloads, bundle.Asset)), ex.Plain); }

        // The same with a release.json that names a file we have the real bytes of.
        var content = Encoding.UTF8.GetBytes("pretend this is the SDK bundle");
        var sha = Convert.ToHexStringLower(SHA256.HashData(content));
        var altered = original.Replace(bundle.Sha256, sha).Replace($"\"size\": {bundle.Size}", $"\"size\": {content.Length}");
        File.WriteAllText(Path.Combine(folder, "release.json"), altered);
        File.WriteAllBytes(bundleFile, content);
        var source2 = ReleaseSource.FromDownloads(folder);
        var release2 = await source2.LoadAsync(CancellationToken.None);
        var downloads2 = Path.Combine(scratch, "dl2");
        await Verified.FetchAsync("the SDK bundle", Path.Combine(downloads2, bundle.Asset), release2.SdkBundle!.Size, release2.SdkBundle.Sha256,
            (destination, progress, token) => source2.FetchAssetAsync(bundle.Asset, destination, progress, token), new Progress<long>(), _ => { }, CancellationToken.None);
        Check("the right bytes are taken from the folder, with no download", File.ReadAllBytes(Path.Combine(downloads2, bundle.Asset)).SequenceEqual(content));

        // A tool archive in the folder's tools\ subfolder is found, as the dev override finds it.
        var tool = release.Tools["ninja"];
        var toolName = Path.GetFileName(new Uri(tool.Url).AbsolutePath);
        Directory.CreateDirectory(Path.Combine(folder, "tools"));
        using (var fake = new FileStream(Path.Combine(folder, "tools", toolName), FileMode.Create)) fake.SetLength(tool.Size);
        Check("a tool archive in tools\\ is found", source2.OfflineFiles().Single(f => f.Name == toolName).Present);

        // The rules of a downloaded release.json are GitHub's: the repository is checked, dev-source.json is ignored.
        File.WriteAllText(Path.Combine(folder, "dev-source.json"), "{\"gitUrl\": \"C:\\\\somewhere\\\\else\"}");
        var source3 = ReleaseSource.FromDownloads(folder);
        await source3.LoadAsync(CancellationToken.None);
        Check("dev-source.json in the player's folder is ignored", source3.GitUrl == $"https://github.com/{ReleaseSource.Repository}.git", source3.GitUrl);
        File.WriteAllText(Path.Combine(folder, "release.json"), original.Replace("\"repository\": \"RenanQueiroz/mcla-recomp\"", "\"repository\": \"someone/else\""));
        try { await ReleaseSource.FromDownloads(folder).LoadAsync(CancellationToken.None); Check("a release.json from another repository is refused", false); }
        catch (SetupException ex) { Check("a release.json from another repository is refused", ex.Plain.Contains("someone/else"), ex.Plain); }
        File.WriteAllText(Path.Combine(folder, "release.json"), original.Replace("\"url\": \"https://github.com/Kitware", "\"url\": \"http://github.com/Kitware"));
        try { await ReleaseSource.FromDownloads(folder).LoadAsync(CancellationToken.None); Check("a tool address that is not https is refused", false); }
        catch (SetupException ex) { Check("a tool address that is not https is refused", ex.Plain.Contains("https"), ex.Plain); }
        File.WriteAllText(Path.Combine(folder, "release.json"), original.Replace("\"asset\": \"" + bundle.Asset + "\"", "\"asset\": \"..\\\\evil.zip\""));
        try { await ReleaseSource.FromDownloads(folder).LoadAsync(CancellationToken.None); Check("an asset name with a path is refused", false); }
        catch (SetupException ex) { Check("an asset name with a path is refused", true, ex.Plain); }

        // The mistakes a player makes.
        try { ReleaseSource.FromDownloads(Path.Combine(scratch, "no such folder")); Check("a folder that is not there is refused", false); }
        catch (SetupException ex) { Check("a folder that is not there is refused", true, ex.Plain); }
        var empty = Path.Combine(scratch, "empty");
        Directory.CreateDirectory(empty);
        try { await ReleaseSource.FromDownloads(empty).LoadAsync(CancellationToken.None); Check("a folder with no release.json says what to download", false); }
        catch (SetupException ex) { Check("a folder with no release.json says what to download", ex.Plain.Contains("Download release.json"), ex.Plain); }
        File.WriteAllText(Path.Combine(empty, "release.json"), "<html>Sign in</html>");
        try { await ReleaseSource.FromDownloads(empty).LoadAsync(CancellationToken.None); Check("a web page saved as release.json is refused", false); }
        catch (SetupException ex) { Check("a web page saved as release.json is refused", true, ex.Plain); }
    }
}

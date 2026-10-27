// Development only: checks of src\Net.cs (the connect logic, the stage messages, the release.json routes) with no window and
// no internet. Everything talks to servers on 127.0.0.1 started here; the addresses that are meant not to answer are faked
// (a connector that hangs) or taken from ranges that nothing routes (100::1 is the IPv6 discard prefix).
//
//   dotnet run --project launcher\dev\net-check -c Release            all the checks
//   dotnet run --project launcher\dev\net-check -c Release -- connect | stages | release
//   dotnet run --project launcher\dev\net-check -c Release -- ui <wizard exe> <screenshot folder> <down|api|rate|downloads> <release.json> [<downloads folder>]
//       the real wizard window against a fake GitHub on 127.0.0.1 that fails the way the scenario says; the window's own
//       screenshots (MCLA_DEV_SHOTS) are the result. Needs a desktop; the window closes itself after 28 s.
//
// Exit code 0 when every check passed.
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;
using MclaLauncher;

static class Program
{
    static int failures, checks;

    static void Section(string name) => Console.WriteLine($"\n== {name}");

    static void Check(bool ok, string what, string? detail = "")
    {
        checks++;
        if (!ok) failures++;
        Console.WriteLine($"{(ok ? "ok  " : "FAIL")} {what}{(detail is { Length: > 0 } ? $"  [{detail}]" : "")}");
    }

    static async Task<int> Main(string[] args)
    {
        var which = args.Length > 0 ? args[0] : "all";
        if (which is "all" or "connect") await Connect();
        if (which is "all" or "stages") await Stages();
        if (which is "all" or "release") await Release();
        if (which == "ui") return await Ui(args);
        Console.WriteLine($"\n{checks - failures} of {checks} checks passed" + (failures > 0 ? $", {failures} FAILED" : ""));
        return failures == 0 ? 0 : 1;
    }

    // ---- helpers ------------------------------------------------------------------------------------------------

    /// A TCP listener that accepts and holds the connections (for the connect tests).
    sealed class Listener : IDisposable
    {
        readonly TcpListener listener;
        readonly ConcurrentBag<TcpClient> held = [];
        public int Port { get; }
        public int Accepted;
        public Listener(IPAddress? address = null)
        {
            listener = new TcpListener(address ?? IPAddress.Loopback, 0);
            listener.Start();
            Port = ((IPEndPoint)listener.LocalEndpoint).Port;
            _ = Task.Run(async () =>
            {
                while (true)
                {
                    try { held.Add(await listener.AcceptTcpClientAsync()); Interlocked.Increment(ref Accepted); }
                    catch (Exception) { return; }
                }
            });
        }
        public void Dispose() { listener.Stop(); foreach (var c in held) c.Dispose(); }
    }

    static int ClosedPort()
    {
        var l = new TcpListener(IPAddress.Loopback, 0);
        l.Start();
        var port = ((IPEndPoint)l.LocalEndpoint).Port;
        l.Stop();
        return port;
    }

    /// An HTTP server on 127.0.0.1.
    sealed class Server : IDisposable
    {
        readonly HttpListener listener;
        public int Port { get; }
        public int Requests;
        public ConcurrentQueue<string> Paths { get; } = new();
        public Func<HttpListenerContext, int, Task> Handler = (ctx, _) => { ctx.Response.Close(); return Task.CompletedTask; };
        public string Url => $"http://127.0.0.1:{Port}";

        public Server()
        {
            var port = Random.Shared.Next(20000, 60000);
            HttpListener? started = null;
            for (var tries = 0; started == null; tries++)
            {
                var candidate = new HttpListener();
                try { candidate.Prefixes.Add($"http://127.0.0.1:{port}/"); candidate.Start(); started = candidate; }
                catch (HttpListenerException) when (tries < 50) { candidate.Close(); port = port >= 65000 ? 20000 : port + 1; }
            }
            listener = started;
            Port = port;
            _ = Task.Run(async () =>
            {
                while (true)
                {
                    HttpListenerContext ctx;
                    try { ctx = await listener.GetContextAsync(); } catch (Exception) { return; }
                    var n = Interlocked.Increment(ref Requests);
                    Paths.Enqueue(ctx.Request.RawUrl ?? "");
                    _ = Task.Run(async () => { try { await Handler(ctx, n); } catch (Exception) { try { ctx.Response.Abort(); } catch (Exception) { } } });
                }
            });
        }

        public void Dispose() { try { listener.Stop(); } catch (Exception) { } listener.Close(); }
    }

    static async Task Reply(HttpListenerContext ctx, int status, string body = "", params (string, string)[] headers)
    {
        ctx.Response.StatusCode = status;
        foreach (var (name, value) in headers) ctx.Response.AddHeader(name, value);
        var bytes = Encoding.UTF8.GetBytes(body);
        ctx.Response.ContentLength64 = bytes.Length;
        await ctx.Response.OutputStream.WriteAsync(bytes);
        ctx.Response.Close();
    }

    static async Task<Exception?> Catch(Func<Task> action)
    {
        try { await action(); return null; } catch (Exception ex) { return ex; }
    }

    // ---- the connect logic ---------------------------------------------------------------------------------------

    static readonly IPAddress V6a = IPAddress.Parse("2001:db8::1"), V6b = IPAddress.Parse("2001:db8::2");
    static readonly IPAddress V4a = IPAddress.Parse("192.0.2.1"), V4b = IPAddress.Parse("192.0.2.2");

    /// Behaviours for the faked addresses: how each address connects (or doesn't), and what happened to it.
    sealed class Fake
    {
        public readonly Dictionary<IPAddress, Func<CancellationToken, Task<Socket>>> By = [];
        public readonly ConcurrentQueue<IPAddress> Started = new();
        public readonly ConcurrentBag<IPAddress> Cancelled = [];
        public readonly ConcurrentBag<Socket> Sockets = [];

        public ConnectOne Connect => async (address, port, ct) =>
        {
            Started.Enqueue(address);
            try { return await By[address](ct); }
            catch (OperationCanceledException) { Cancelled.Add(address); throw; }
        };

        /// Never answers (a black hole).
        public static Func<CancellationToken, Task<Socket>> Hang => async ct => { await Task.Delay(Timeout.Infinite, ct); throw new InvalidOperationException(); };
        public static Func<CancellationToken, Task<Socket>> Refuse => _ => throw new SocketException((int)SocketError.ConnectionRefused);
        public static Func<CancellationToken, Task<Socket>> Unreachable => _ => throw new SocketException((int)SocketError.NetworkUnreachable);

        /// Like Real, but deaf to cancellation: a connection attempt that succeeds after it has lost.
        public Func<CancellationToken, Task<Socket>> RealDeaf(int port, int delay) => async _ =>
        {
            await Task.Delay(delay);
            var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            Sockets.Add(socket);
            await socket.ConnectAsync(IPAddress.Loopback, port);
            return socket;
        };

        /// Connects to the local listener after `delay` (the address it is registered under is only a label).
        public Func<CancellationToken, Task<Socket>> Real(int port, int delay = 0) => async ct =>
        {
            if (delay > 0) await Task.Delay(delay, ct);
            var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            Sockets.Add(socket);
            try { await socket.ConnectAsync(IPAddress.Loopback, port, ct); return socket; }
            catch { socket.Dispose(); throw; }
        };
    }

    static async Task<(ConnectedSocket? Won, Exception? Error, double Ms, NetOperation Op)> Race(IReadOnlyList<IPAddress> addresses, Fake fake, int port = 443,
        TimeSpan? budget = null, CancellationToken ct = default)
    {
        var op = new NetOperation("test");
        var watch = Stopwatch.StartNew();
        try
        {
            var won = await HappyEyeballs.ConnectAsync("name.test", addresses, port, ct, op, budget: budget, connect: fake.Connect, allowIPv6: true);
            return (won, null, watch.Elapsed.TotalMilliseconds, op);
        }
        catch (Exception ex) { return (null, ex, watch.Elapsed.TotalMilliseconds, op); }
    }

    static async Task Connect()
    {
        Section("the order the addresses are tried in");
        Check(HappyEyeballs.Order([V6a, V4a, V6b], true).SequenceEqual([V6a, V4a, V6b]), "IPv6 first, then IPv4, then the second IPv6");
        Check(HappyEyeballs.Order([V4a, V6a, V4b], true).SequenceEqual([V4a, V6a, V4b]), "a name whose first address is IPv4 starts with IPv4");
        Check(HappyEyeballs.Order([V6a, V4a, V4b, V6b], true).SequenceEqual([V6a, V4a, V6b, V4b]), "the families take turns");
        Check(HappyEyeballs.Order([V6a, V4a, V6b], false).SequenceEqual([V4a]), "no IPv6 on the PC: only IPv4");
        Check(HappyEyeballs.Order([V4a, V4a, IPAddress.Parse("::ffff:192.0.2.1")], true).SequenceEqual([V4a]), "duplicates, and an IPv4 written as IPv6, count once");

        using var listener = new Listener();

        Section("IPv6 black-holed, IPv4 works (the case of issue 13)");
        {
            var fake = new Fake { By = { [V6a] = Fake.Hang, [V4a] = default! } };
            fake.By[V4a] = fake.Real(listener.Port);
            var (won, error, ms, op) = await Race([V6a, V4a], fake);
            Check(won?.Address.Equals(V4a) == true && error == null, "IPv4 wins", $"{ms:0} ms");
            Check(ms is >= 230 and <= 1500, "after the 250 ms head start of IPv6, not after a connect timeout", $"{ms:0} ms");
            await Task.Delay(100);
            Check(fake.Cancelled.Contains(V6a), "the hanging IPv6 attempt was cancelled");
            Check(op.Events.Any(e => e.Contains("2001:db8::1") && e.Contains("started at 0 ms")) && op.Events.Any(e => e.Contains("192.0.2.1") && e.Contains("started at 2")),
                  "the log has the start of each attempt with its time");
            won?.Socket.Dispose();
        }

        Section("real addresses: 100::1 (IPv6 discard prefix, routed nowhere) and the local listener");
        {
            var op = new NetOperation("real");
            var watch = Stopwatch.StartNew();
            var won = await HappyEyeballs.ConnectAsync("name.test", [IPAddress.Parse("100::1"), IPAddress.Loopback], listener.Port, default, op, allowIPv6: true);
            Check(won.Address.Equals(IPAddress.Loopback), "IPv4 wins with real sockets", $"{watch.Elapsed.TotalMilliseconds:0} ms");
            Check(watch.Elapsed.TotalMilliseconds < 1000, "well before any connect timeout", $"{watch.Elapsed.TotalMilliseconds:0} ms");
            foreach (var line in op.Events.Where(e => e.Contains("100::1"))) Console.WriteLine("      " + line);
            won.Socket.Dispose();
        }

        Section("IPv6 healthy: it wins and IPv4 is never started");
        try
        {
            using var v6 = new Listener(IPAddress.IPv6Loopback);
            var op = new NetOperation("v6");
            var watch = Stopwatch.StartNew();
            var won = await HappyEyeballs.ConnectAsync("name.test", [IPAddress.IPv6Loopback, IPAddress.Loopback], v6.Port, default, op, allowIPv6: true);
            Check(won.Address.Equals(IPAddress.IPv6Loopback), "a real IPv6 connection works", $"{watch.Elapsed.TotalMilliseconds:0} ms");
            Check(!op.Events.Any(e => e.Contains("127.0.0.1") && e.Contains("started")), "the IPv4 attempt was never started");
            won.Socket.Dispose();
        }
        catch (SocketException ex) { Console.WriteLine($"      (no IPv6 loopback on this PC: {ex.SocketErrorCode})"); }

        Section("the first family answers at once");
        {
            var fake = new Fake { By = { [V4a] = default!, [V6a] = Fake.Hang } };
            fake.By[V4a] = fake.Real(listener.Port);
            var (won, _, ms, _) = await Race([V4a, V6a], fake);
            Check(won?.Address.Equals(V4a) == true && ms < 500 && !fake.Started.Contains(V6a), "IPv4 first and healthy: IPv6 is never started", $"{ms:0} ms");
            won?.Socket.Dispose();
        }

        Section("the first attempt fails at once (refused, unreachable): the next starts at once, not after 250 ms");
        {
            var fake = new Fake { By = { [V6a] = Fake.Unreachable, [V4a] = default! } };
            fake.By[V4a] = fake.Real(listener.Port);
            var (won, _, ms, _) = await Race([V6a, V4a], fake);
            Check(won?.Address.Equals(V4a) == true && ms < 500, "IPv4 right after the IPv6 failure", $"{ms:0} ms");
            won?.Socket.Dispose();
        }

        Section("a slow IPv6 that works beats a hanging IPv4");
        {
            var fake = new Fake { By = { [V6a] = default!, [V4a] = Fake.Hang } };
            fake.By[V6a] = fake.Real(listener.Port, delay: 400);
            var (won, _, ms, _) = await Race([V6a, V4a], fake);
            Check(won?.Address.Equals(V6a) == true && ms is >= 380 and < 1500, "IPv6 connecting at 400 ms wins", $"{ms:0} ms");
            await Task.Delay(100);
            Check(fake.Cancelled.Contains(V4a), "the hanging IPv4 attempt was cancelled");
            won?.Socket.Dispose();
        }

        Section("a loser that connects late is closed, not leaked");
        {
            var fake = new Fake { By = { [V6a] = default!, [V4a] = default! } };
            fake.By[V6a] = fake.RealDeaf(listener.Port, 400);   // connects after IPv4 has won, whatever it is told
            fake.By[V4a] = fake.Real(listener.Port);
            var (won, _, ms, _) = await Race([V6a, V4a], fake);
            Check(won?.Address.Equals(V4a) == true && ms is >= 230 and < 700, "IPv4 starts at 250 ms and wins", $"{ms:0} ms");
            await Task.Delay(500);
            var loser = fake.Sockets.FirstOrDefault(s => s != won?.Socket);
            Check(loser != null && loser.SafeHandle.IsClosed, "the socket of the attempt that lost is closed");
            won?.Socket.Dispose();
        }

        Section("a second address of the first family comes after the first of the other");
        {
            var fake = new Fake { By = { [V6a] = Fake.Hang, [V4a] = Fake.Hang, [V6b] = default!, [V4b] = Fake.Hang } };
            fake.By[V6b] = fake.Real(listener.Port);
            var (won, _, ms, _) = await Race([V6a, V6b, V4a, V4b], fake);
            Check(fake.Started.ToArray().Take(3).SequenceEqual([V6a, V4a, V6b]) && won?.Address.Equals(V6b) == true && ms is >= 480 and < 1500, "started in the order IPv6, IPv4, IPv6, 250 ms apart; the third wins", $"{ms:0} ms");
            won?.Socket.Dispose();
        }

        Section("nothing connects");
        {
            var fake = new Fake { By = { [V6a] = Fake.Hang, [V4a] = Fake.Hang } };
            var (_, error, ms, _) = await Race([V6a, V4a], fake, budget: TimeSpan.FromMilliseconds(900));
            Check(error is ConnectFailure { Stage: NetStage.Connect } cf && cf.Reason.Contains("2001:db8::1") && cf.Reason.Contains("192.0.2.1") && cf.Reason.Contains("no answer"),
                  "both hanging: a ConnectFailure that names both addresses and says no answer", $"{ms:0} ms, {error?.Message}");
            Check(ms is >= 850 and < 2500, "it gives up at the budget", $"{ms:0} ms");

            fake = new Fake { By = { [V6a] = Fake.Unreachable, [V4a] = Fake.Refuse } };
            (_, error, ms, _) = await Race([V6a, V4a], fake);
            Check(error is ConnectFailure cf2 && cf2.Reason.Contains("network unreachable") && cf2.Reason.Contains("refused") && ms < 600, "both failing: the reasons of both, at once", $"{ms:0} ms, {error?.Message}");

            fake = new Fake { By = { [V4a] = Fake.Hang } };
            using var cts = new CancellationTokenSource(300);
            (_, error, ms, _) = await Race([V4a], fake, ct: cts.Token);
            Check(error is OperationCanceledException && ms < 1500, "cancelling stops it", $"{ms:0} ms");

            (_, error, _, _) = await Race([], new Fake());
            Check(error is ConnectFailure, "no address at all is a ConnectFailure");
        }

        Section("the name lookup");
        {
            var op = new NetOperation("dns");
            var literal = await HappyEyeballs.ResolveAsync("127.0.0.1", 80, op, default);
            Check(literal.Length == 1 && literal[0].Equals(IPAddress.Loopback) && op.Events.Any(e => e.Contains("no lookup")), "an address is not looked up");

            var error = await Catch(() => HappyEyeballs.ResolveAsync("nonexistent-host.invalid", 443, op, default));
            Check(error is ConnectFailure { Stage: NetStage.Lookup }, "a name that does not exist is a Lookup failure", error?.Message ?? "");

            error = await Catch(() => HappyEyeballs.ResolveAsync("slow.test", 443, op, default, TimeSpan.FromMilliseconds(300), async (_, ct) => { await Task.Delay(Timeout.Infinite, ct); return []; }));
            Check(error is ConnectFailure { Stage: NetStage.Lookup } e && e.Reason.Contains("didn't answer"), "a DNS server that does not answer is a Lookup failure after the budget", error?.Message ?? "");
        }

        Section("through the HTTP handler, with a name that has a black-holed IPv6 address and a good IPv4 one");
        {
            using var server = new Server();
            server.Handler = (ctx, _) => Reply(ctx, 200, "hello");
            var fake = new Fake { By = { [V6a] = Fake.Hang, [IPAddress.Loopback] = default! } };
            fake.By[IPAddress.Loopback] = fake.Real(server.Port);
            HappyEyeballs.ResolverOverride = (_, _) => Task.FromResult(new[] { V6a, IPAddress.Loopback });
            HappyEyeballs.ConnectorOverride = fake.Connect;
            try
            {
                using var client = NetClient.Create();
                var op = new NetOperation("end to end");
                var watch = Stopwatch.StartNew();
                using var response = await NetClient.GetAsync($"http://github.test:{server.Port}/x", op, new NetRequest { Client = client, AllowPlainHttp = true }, default);
                var text = await NetClient.ReadTextAsync(response, op, TimeSpan.FromSeconds(5), 1000, default);
                Check(text == "hello" && watch.Elapsed.TotalMilliseconds is >= 230 and < 1500, "the request completes about 250 ms after it started, not after a timeout", $"{watch.Elapsed.TotalMilliseconds:0} ms");
            }
            finally { HappyEyeballs.ResolverOverride = null; HappyEyeballs.ConnectorOverride = null; }
        }

        Section("the system's name 'localhost' (IPv6 first, the server is IPv4 only)");
        {
            using var server = new Server();
            server.Handler = (ctx, _) => Reply(ctx, 200, "local");
            using var client = NetClient.Create();
            var op = new NetOperation("localhost");
            using var response = await NetClient.GetAsync($"http://localhost:{server.Port}/x", op, new NetRequest { Client = client, AllowPlainHttp = true }, default);
            Check(await NetClient.ReadTextAsync(response, op, TimeSpan.FromSeconds(5), 1000, default) == "local", "reachable");
        }
    }

    // ---- the stage messages ---------------------------------------------------------------------------------------

    static NetRequest Plain(HttpClient client, double answerSeconds = 2) => new() { Client = client, AllowPlainHttp = true, AnswerTimeout = TimeSpan.FromSeconds(answerSeconds) };

    static async Task<NetworkFailure?> Fail(string url, HttpClient client, double answerSeconds = 2, bool read = false)
    {
        var op = new NetOperation("test fetch");
        try
        {
            using var response = await NetClient.GetAsync(url, op, Plain(client, answerSeconds), default);
            if (!response.IsSuccessStatusCode) throw NetClient.StatusFailure(response, op);
            if (read) await NetClient.ReadTextAsync(response, op, TimeSpan.FromMilliseconds(600), 1 << 20, default);
            return null;
        }
        catch (NetworkFailure f) { return f; }
    }

    static async Task Stages()
    {
        using var client = NetClient.Create();

        Section("the name does not exist");
        var f = await Fail("http://nonexistent-host.invalid/file", client);
        Check(f is { Stage: NetStage.Lookup, Hop: 0 } && f.Plain.Contains("DNS lookup for nonexistent-host.invalid"), "stage Lookup, naming the host", f?.Plain);

        Section("the connection is refused");
        f = await Fail($"http://127.0.0.1:{ClosedPort()}/file", client, answerSeconds: 8);
        Check(f is { Stage: NetStage.Connect, Hop: 0 } && f.Plain.StartsWith("Couldn't connect to 127.0.0.1") && f.Plain.Contains("refused"), "stage Connect, naming the host and why", f?.Plain);
        Check(f?.Report().Contains("connect 127.0.0.1 (IPv4) port") == true && f.Report().Contains("proxy in use:") && f.Report().Contains("IPv6 on this PC:") && f.Report().Contains("ConnectFailure"),
              "the report has the addresses tried, the proxy, IPv6 and the exception chain");

        Section("the server accepts the connection and never answers");
        using (var silent = new Listener())
        {
            var watch = Stopwatch.StartNew();
            f = await Fail($"http://127.0.0.1:{silent.Port}/file", client, answerSeconds: 0.8);
            Check(f is { Stage: NetStage.Answer } && f.Plain.Contains("didn't answer within 0.8 s"), "stage Answer", $"{watch.Elapsed.TotalMilliseconds:0} ms: {f?.Plain}");
        }

        Section("GitHub answers with a redirect to a host that cannot be reached (the case of the download host)");
        using (var origin = new Server())
        {
            var closed = ClosedPort();
            origin.Handler = (ctx, _) => Reply(ctx, 302, "", ("Location", $"http://127.0.0.1:{closed}/asset?sig=SECRET-TOKEN"));
            f = await Fail($"{origin.Url}/releases/latest/download/release.json", client, answerSeconds: 8);
            Check(f is { Stage: NetStage.Connect, Hop: 1 } && f.Plain.Contains("the download host 127.0.0.1") && f.Plain.Contains("GitHub itself answered"), "stage Connect on hop 1, naming the download host", f?.Plain);
            var report = f?.Report() ?? "";
            Check(report.Contains("redirected to 127.0.0.1") && report.Contains("(redirect 1)") && !report.Contains("SECRET-TOKEN"), "the report has the redirect and not the signed query", "");
        }

        Section("redirects");
        using (var a = new Server())
        using (var b = new Server())
        {
            a.Handler = (ctx, _) => Reply(ctx, 302, "", ("Location", $"{b.Url}/file?x=1"));
            b.Handler = (ctx, _) => Reply(ctx, 200, "the file");
            var op = new NetOperation("redirect");
            using var response = await NetClient.GetAsync($"{a.Url}/start", op, Plain(client), default);
            Check(await NetClient.ReadTextAsync(response, op, TimeSpan.FromSeconds(2), 1000, default) == "the file" && b.Paths.Contains("/file?x=1"), "a redirect to another host is followed");

            a.Handler = (ctx, _) => Reply(ctx, 302, "", ("Location", "/again"));
            f = await Fail($"{a.Url}/start", client);
            Check(f is { Stage: NetStage.Redirect, Retriable: false } && f.Plain.Contains("more than 5 redirects"), "a loop of redirects stops", f?.Plain);

            a.Handler = (ctx, _) => Reply(ctx, 302, "");
            f = await Fail($"{a.Url}/start", client);
            Check(f is { Stage: NetStage.Redirect } && f.Plain.Contains("names no address"), "a redirect with no address", f?.Plain);
        }
        f = await Fail("http://127.0.0.1:1/plain", NetClient.Create());
        // (this one goes through GetAsync without AllowPlainHttp)
        var strict = new NetworkFailure[1];
        try { using var r = await NetClient.GetAsync("http://127.0.0.1:1/plain", new NetOperation("http"), new NetRequest { Client = client }, default); }
        catch (NetworkFailure ex) { strict[0] = ex; }
        Check(strict[0] is { Stage: NetStage.Redirect, Retriable: false } && strict[0].Plain.Contains("isn't https"), "plain http is refused unless a test allows it", strict[0]?.Plain);

        Section("the server's answer");
        using (var server = new Server())
        {
            server.Handler = (ctx, _) => Reply(ctx, 404);
            f = await Fail($"{server.Url}/x", client);
            Check(f is { Stage: NetStage.Status, Retriable: false, StatusCode: HttpStatusCode.NotFound } && f.Plain.Contains("404"), "404 is final", f?.Plain);
            server.Handler = (ctx, _) => Reply(ctx, 503);
            f = await Fail($"{server.Url}/x", client);
            Check(f is { Stage: NetStage.Status, Retriable: true } && f.Plain.Contains("503"), "503 may be tried again", f?.Plain);
            var reset = DateTimeOffset.Now.AddMinutes(40);
            server.Handler = (ctx, _) => Reply(ctx, 403, "{}", ("X-RateLimit-Remaining", "0"), ("X-RateLimit-Reset", reset.ToUnixTimeSeconds().ToString()));
            f = await Fail($"{server.Url}/x", client);
            Check(f is { RateLimited: true } && f.Plain.Contains("60 an hour") && f.Plain.Contains(reset.ToLocalTime().ToString("HH:mm")), "GitHub's rate limit has its own message with the time it resets", f?.Plain);
            server.Handler = (ctx, _) => Reply(ctx, 403, "{}");
            f = await Fail($"{server.Url}/x", client);
            Check(f is { RateLimited: false } && f.Plain.Contains("403"), "another 403 is not called a rate limit", f?.Plain);
            server.Handler = (ctx, _) => Reply(ctx, 407);
            f = await Fail($"{server.Url}/x", client);
            Check(f != null && f.Plain.Contains("asks for a sign-in"), "407 says the proxy wants a sign-in", f?.Plain);
        }

        Section("reading the body");
        using (var server = new Server())
        {
            server.Handler = async (ctx, _) =>
            {
                ctx.Response.ContentLength64 = 1000;
                await ctx.Response.OutputStream.WriteAsync(new byte[100]);
                await ctx.Response.OutputStream.FlushAsync();
                await Task.Delay(5000);
            };
            f = await Fail($"{server.Url}/x", client, read: true);
            Check(f is { Stage: NetStage.Read } && f.Plain.Contains("stopped sending data"), "a stall is a Read failure", f?.Plain);
            server.Handler = async (ctx, _) =>
            {
                ctx.Response.ContentLength64 = 1000;
                await ctx.Response.OutputStream.WriteAsync(new byte[100]);
                await ctx.Response.OutputStream.FlushAsync();
                await Task.Delay(200);
                ctx.Response.Abort();
            };
            f = await Fail($"{server.Url}/x", client, read: true);
            Check(f is { Stage: NetStage.Read } && f.Plain.Contains("was lost"), "a connection cut in the middle is a Read failure", f?.Plain);
        }

        Section("through a proxy: the connect callback connects to the proxy");
        using (var proxy = new Server())
        {
            proxy.Handler = (ctx, _) => Reply(ctx, 200, "via the proxy");
            using var proxied = NetClient.Create(NetClient.CreateHandler(new WebProxy($"http://127.0.0.1:{proxy.Port}")));
            var op = new NetOperation("proxy");
            using var response = await NetClient.GetAsync("http://does-not-exist.invalid/some/path", op, Plain(proxied), default);
            Check(await NetClient.ReadTextAsync(response, op, TimeSpan.FromSeconds(2), 1000, default) == "via the proxy" && proxy.Paths.Contains("http://does-not-exist.invalid/some/path"),
                  "the request reached the proxy with the full address, and the name was not looked up here");
            Check(!op.Events.Any(e => e.Contains("DNS does-not-exist")) && op.Events.Any(e => e.Contains("connect 127.0.0.1 (IPv4)")), "the log shows a connection to the proxy only");

            var dead = ClosedPort();
            using var broken = NetClient.Create(NetClient.CreateHandler(new WebProxy($"http://127.0.0.1:{dead}")));
            f = await Fail("http://does-not-exist.invalid/x", broken, answerSeconds: 8);
            Check(f is { Stage: NetStage.Connect } && f.Plain.Contains("the proxy 127.0.0.1"), "a proxy that cannot be reached is named as the proxy", f?.Plain);
        }
    }

    // ---- release.json: the routes ----------------------------------------------------------------------------------

    static async Task Release()
    {
        const string repo = "owner/repo";
        const string json = "{\"schema\":1}";
        using var client = NetClient.Create();
        var timing = new ReleaseFetch.Timing
        {
            Client = client, FirstAnswer = TimeSpan.FromSeconds(2), LaterAnswer = TimeSpan.FromSeconds(2),
            RetryPause = TimeSpan.FromMilliseconds(50), Stall = TimeSpan.FromSeconds(1),
        };
        using var web = new Server();
        using var api = new Server();
        var endpoints = new ReleaseEndpoints(web.Url, api.Url, repo, AllowPlainHttp: true);
        var direct = $"/{repo}/releases/latest/download/release.json";
        var tagged = $"/{repo}/releases/download/v1/release.json";
        var apiPath = $"/repos/{repo}/releases/latest";
        string ApiBody(string? assetUrl = null) => "{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"other.zip\",\"browser_download_url\":\"" + web.Url + "/x\"},{\"name\":\"release.json\",\"browser_download_url\":\"" + (assetUrl ?? web.Url + tagged) + "\"}]}";
        var directBehaviour = (Func<HttpListenerContext, int, Task>)((ctx, n) => Reply(ctx, 200, json));
        web.Handler = (ctx, n) => ctx.Request.RawUrl == tagged ? Reply(ctx, 200, json) : directBehaviour(ctx, n);
        api.Handler = (ctx, n) => ctx.Request.RawUrl == apiPath && ctx.Request.Headers["Accept"]?.Contains("github+json") == true ? Reply(ctx, 200, ApiBody()) : Reply(ctx, 404);

        async Task<(string? Json, ReleaseRoute Route, NetworkFailure? Failure, List<string> Said, NetOperation Op)> Run()
        {
            web.Requests = 0; api.Requests = 0;
            var said = new List<string>();
            var op = new NetOperation("find the latest release");
            try
            {
                var (text, route) = await ReleaseFetch.FetchAsync(endpoints, op, said.Add, default, timing);
                return (text, route, null, said, op);
            }
            catch (NetworkFailure failure) { return (null, default, failure, said, op); }
        }

        Section("release.json: the direct address");
        var r = await Run();
        Check(r.Json == json && r.Route == ReleaseRoute.Direct && web.Requests == 1 && api.Requests == 0, "answers at the first try; the API is not asked");

        Section("a failure of the first try: one automatic retry");
        directBehaviour = (ctx, n) => n == 1 ? Reply(ctx, 500) : Reply(ctx, 200, json);
        r = await Run();
        Check(r.Json == json && r.Route == ReleaseRoute.SecondTry && web.Requests == 2 && api.Requests == 0 && r.Said.SequenceEqual(["Looking up the latest release…", "Trying again…"]), "the second try works; the page was told", string.Join(" | ", r.Said));

        Section("the direct address does not work at all: the API names the file");
        directBehaviour = (ctx, n) => Reply(ctx, 500);
        r = await Run();
        Check(r.Json == json && r.Route == ReleaseRoute.Api && api.Requests == 1, "release.json arrives by the API's browser_download_url", $"web {web.Requests}, api {api.Requests}");
        Check(r.Said.Last() == "Asking GitHub's API instead…" && r.Op.Events.Any(e => e.Contains("route 2")), "the page was told, and the log has the route");

        Section("the direct address is a 404: final, nothing else is tried");
        directBehaviour = (ctx, n) => Reply(ctx, 404);
        r = await Run();
        Check(r.Failure is { StatusCode: HttpStatusCode.NotFound } && web.Requests == 1 && api.Requests == 0, "no retry, no API");

        Section("the direct address fails and the API's limit is used up");
        directBehaviour = (ctx, n) => Reply(ctx, 502);
        var reset = DateTimeOffset.Now.AddMinutes(25);
        var apiBefore = api.Handler;
        api.Handler = (ctx, n) => Reply(ctx, 403, "{}", ("X-RateLimit-Remaining", "0"), ("X-RateLimit-Reset", reset.ToUnixTimeSeconds().ToString()));
        r = await Run();
        Check(r.Failure is { Also.Count: 1 } fx && fx.Also[0] is NetworkFailure { RateLimited: true } && fx.Plain.Contains("502") && fx.Plain.Contains("limit of requests") && fx.Plain.Contains(reset.ToLocalTime().ToString("HH:mm")),
              "the message is the direct failure plus the API's own message about its limit", r.Failure?.Plain);
        Check(r.Failure?.Report().Contains("also failed:") == true && r.Failure.Report().Contains("route 1") && r.Failure.Report().Contains("route 2"), "the report has both routes");

        Section("the API names an address that is not this repository's release");
        api.Handler = (ctx, n) => Reply(ctx, 200, ApiBody("https://evil.example/release.json"));
        r = await Run();
        Check(r.Failure is { Also.Count: 1 } && r.Failure.Also[0].Message.Contains("isn't this repository's release"), "refused", (r.Failure?.Also.FirstOrDefault())?.Message);

        Section("the API's answer is not a list, or lists no release.json");
        api.Handler = (ctx, n) => Reply(ctx, 200, "<html>sign in</html>");
        r = await Run();
        Check(r.Failure is { Also.Count: 1 } && r.Failure.Also[0].Message.Contains("isn't a list"), "not JSON", (r.Failure?.Also.FirstOrDefault())?.Message);
        api.Handler = (ctx, n) => Reply(ctx, 200, "{\"assets\":[]}");
        r = await Run();
        Check(r.Failure is { Also.Count: 1 } && r.Failure.Also[0].Message.Contains("no release.json"), "no release.json among the assets", (r.Failure?.Also.FirstOrDefault())?.Message);
        api.Handler = apiBefore;

        Section("the web host cannot be reached at all (connection refused), the API cannot either");
        var patient = timing with { FirstAnswer = TimeSpan.FromSeconds(8), LaterAnswer = TimeSpan.FromSeconds(8) };   // (a refused connection takes about 2 s on Windows)
        var dead = new ReleaseEndpoints($"http://127.0.0.1:{ClosedPort()}", $"http://127.0.0.1:{ClosedPort()}", repo, AllowPlainHttp: true);
        var op2 = new NetOperation("find the latest release");
        var watch = Stopwatch.StartNew();
        var error = await Catch(() => ReleaseFetch.FetchAsync(dead, op2, null, default, patient));
        Check(error is NetworkFailure { Stage: NetStage.Connect, Also.Count: 1 } nf && nf.Plain.Contains("Couldn't connect to 127.0.0.1") && nf.Plain.Contains("Asking GitHub's API instead didn't work either"),
              "the message is the direct failure plus a note about the API", $"{watch.Elapsed.TotalMilliseconds:0} ms; {error?.Message}");

        Section("a hanging server: every route times out");
        using (var silent = new Listener())
        {
            var slow = new ReleaseEndpoints($"http://127.0.0.1:{silent.Port}", $"http://127.0.0.1:{silent.Port}", repo, AllowPlainHttp: true);
            var quick = timing with { FirstAnswer = TimeSpan.FromMilliseconds(400), LaterAnswer = TimeSpan.FromMilliseconds(400) };
            watch.Restart();
            error = await Catch(() => ReleaseFetch.FetchAsync(slow, new NetOperation("find the latest release"), null, default, quick));
            Check(error is NetworkFailure { Stage: NetStage.Answer } && error.Message.Contains("didn't answer within 0.4 s") && watch.Elapsed.TotalSeconds < 6, "stage Answer, three routes of 0.4 s each", $"{watch.Elapsed.TotalSeconds:0.0} s");
        }

        Section("cancelling");
        directBehaviour = async (ctx, n) => { await Task.Delay(3000); await Reply(ctx, 200, json); };
        using (var cts = new CancellationTokenSource(300))
            error = await Catch(() => ReleaseFetch.FetchAsync(endpoints, new NetOperation("x"), null, cts.Token, timing));
        Check(error is OperationCanceledException, "a cancelled fetch ends with OperationCanceledException, not a network error", error?.GetType().Name);
    }

    // ---- the real window ------------------------------------------------------------------------------------------

    static async Task<int> Ui(string[] args)
    {
        var exe = Path.GetFullPath(args[1]);
        var shots = Path.GetFullPath(args[2]);
        var scenario = args[3];
        var releaseJson = File.ReadAllText(args[4]);
        var downloads = args.Length > 5 ? Path.GetFullPath(args[5]) : null;
        const string repo = "RenanQueiroz/mcla-recomp";
        using var web = new Server();
        using var api = new Server();
        var directPath = $"/{repo}/releases/latest/download/release.json";
        var taggedPath = $"/{repo}/releases/download/v0.1.3/release.json";
        var apiPath = $"/repos/{repo}/releases/latest";
        var apiBody = "{\"tag_name\":\"v0.1.3\",\"assets\":[{\"name\":\"release.json\",\"browser_download_url\":\"" + web.Url + taggedPath + "\"}]}";
        web.Handler = (ctx, _) => ctx.Request.RawUrl == taggedPath ? Reply(ctx, 200, releaseJson) : scenario is "api" or "rate" ? Reply(ctx, 502) : Reply(ctx, 200, releaseJson);
        var reset = DateTimeOffset.Now.AddMinutes(37).ToUnixTimeSeconds().ToString();
        api.Handler = (ctx, _) => scenario == "rate" ? Reply(ctx, 403, "{}", ("X-RateLimit-Remaining", "0"), ("X-RateLimit-Reset", reset)) : ctx.Request.RawUrl == apiPath ? Reply(ctx, 200, apiBody) : Reply(ctx, 404);
        // "down": nothing listens at the addresses. "downloads": the window is told the folder, GitHub is not asked at all.
        var webUrl = scenario is "down" or "downloads" ? $"http://127.0.0.1:{ClosedPort()}" : web.Url;
        var apiUrl = scenario is "down" or "downloads" ? $"http://127.0.0.1:{ClosedPort()}" : api.Url;
        Directory.CreateDirectory(shots);
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
        start.Environment["MCLA_GITHUB_WEB"] = webUrl;
        start.Environment["MCLA_GITHUB_API"] = apiUrl;
        start.Environment["MCLA_DEV_SHOTS"] = shots;
        start.ArgumentList.Add("--quit-after=28");
        if (scenario == "downloads" && downloads != null) start.ArgumentList.Add($"--downloads={downloads}");
        Console.WriteLine($"starting {exe}: scenario {scenario}, fake GitHub at {webUrl} and {apiUrl}");
        using var process = Process.Start(start)!;
        using var limit = new CancellationTokenSource(TimeSpan.FromSeconds(90));
        try { await process.WaitForExitAsync(limit.Token); }
        catch (OperationCanceledException) { process.Kill(true); Console.WriteLine("the window did not close in time and was ended"); return 1; }
        Console.WriteLine($"the window closed (exit {process.ExitCode}); web requests {web.Requests}, api requests {api.Requests}");
        Console.WriteLine(string.Join(", ", Directory.GetFiles(shots, "*.png").Select(Path.GetFileName)));
        return 0;
    }
}

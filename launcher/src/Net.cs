// Everything the launcher and the setup wizard do over HTTP goes through here, so that they connect the same way and fail
// in the same words. Depends on nothing else in the launcher (the check program launcher\dev\net-check compiles this file alone).
//
// Why it exists (fork issue 13, "GitHub didn't answer in time to find the latest release"): .NET's SocketsHttpHandler connects to
// the addresses of a name in the order DNS gave them, one after the other, with no fallback while the first one is still being
// tried. On a network that advertises IPv6 but doesn't route it, the first address (IPv6) hangs until the connect timeout while
// a browser, which tries the other family after a fraction of a second, has the page already. So:
//
//   HappyEyeballs   a ConnectCallback that resolves all the addresses of the name, starts the first family, starts the other
//                   family 250 ms later if nothing has connected (at once if the first attempt failed), takes the first socket
//                   that connects and drops the rest (RFC 8305, without its DNS races). With a proxy the handler asks the callback
//                   for the proxy's address, so the same logic connects to the proxy.
//   NetClient       the one HttpClient (system proxy, the Windows login for proxies that ask for it, no automatic redirects:
//                   GetAsync follows them itself, https only, so that a failure says which host it was on).
//   NetOperation    what one fetch did (addresses tried and their timings, redirects, proxy, errors) and the text "Copy details" gives.
//   NetworkFailure  the exception: names the stage that failed (looking up a name, connecting, the secure handshake, waiting for
//                   the answer, reading) and the host, in words for the player; Report() has the rest.
//   ReleaseFetch    release.json: the direct address, one more try, then GitHub's API (api.github.com/repos/<repo>/releases/latest).
using System.Diagnostics;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Authentication;
using System.Text;
using System.Text.Json;

namespace MclaLauncher;

/// Where in a request something went wrong.
enum NetStage
{
    /// Turning a name into addresses.
    Lookup,
    /// Opening the connection (to the host, or to the proxy).
    Connect,
    /// The secure (TLS) handshake.
    Secure,
    /// Waiting for the answer to the request.
    Answer,
    /// The server answered, but not with the file (404, 403, 500, ...).
    Status,
    /// Reading the body of an answer.
    Read,
    /// Following a redirect.
    Redirect,
}

/// What one fetch did, for the error and for "Copy details". Thread-safe: the connect callback writes to it from the pool's threads.
sealed class NetOperation(string what)
{
    readonly object gate = new();
    readonly List<string> events = [];
    readonly List<Uri> visited = [];
    readonly Stopwatch clock = Stopwatch.StartNew();
    int stage;
    string host = "";
    int hop;
    string origin = "";

    /// "find the latest release", "download ninja-win.zip", ...
    public string What { get; } = what;
    public DateTime Started { get; } = DateTime.Now;
    public double Seconds => clock.Elapsed.TotalSeconds;

    /// What the connection was doing last (set by the connect callback and by GetAsync).
    public NetStage Stage { get => (NetStage)Volatile.Read(ref stage); set => Volatile.Write(ref stage, (int)value); }
    /// The host of the request in progress, and how many redirects led to it (0 for the address that was asked for).
    public string Host { get { lock (gate) return host; } }
    public int Hop { get { lock (gate) return hop; } }
    /// How many redirects led to the host the request is on now, when it is another one than the host that was asked
    /// (github.com redirecting to github.com is not a download host); 0 otherwise.
    public int DownloadHop { get { lock (gate) return hop > 0 && !string.Equals(origin, currentAuthority, StringComparison.OrdinalIgnoreCase) ? hop : 0; } }
    string currentAuthority = "";

    public void Log(string line)
    {
        lock (gate) events.Add($"[{clock.Elapsed.TotalSeconds,7:0.000} s] {line}");
    }

    public string[] Events { get { lock (gate) return [.. events]; } }

    internal void BeginHop(int number, Uri uri)
    {
        lock (gate)
        {
            hop = number;
            host = uri.Host;
            currentAuthority = uri.Authority;
            if (number == 0) origin = uri.Authority;
            if (!visited.Any(v => v.Scheme == uri.Scheme && v.Host == uri.Host && v.Port == uri.Port)) visited.Add(new UriBuilder(uri.Scheme, uri.Host, uri.Port).Uri);
        }
        Stage = NetStage.Lookup;
    }

    internal void AddEvents(NetOperation other) { foreach (var line in other.Events) Log("  " + line); }

    /// The text of "Copy details": no addresses with a query string (signed download links), nothing of the player's but the
    /// proxy's address.
    public string Report(Exception? failure = null)
    {
        Uri[] hosts;
        lock (gate) hosts = [.. visited];
        var text = new StringBuilder();
        text.AppendLine("Midnight Club: Los Angeles launcher - network details");
        text.AppendLine($"what: {What}");
        var offset = TimeZoneInfo.Local.GetUtcOffset(Started);
        text.AppendLine($"when: {Started:yyyy-MM-dd HH:mm:ss} (UTC{(offset < TimeSpan.Zero ? "-" : "+")}{offset:hh\\:mm}), {Seconds:0.0} s");
        text.AppendLine($"program: {System.Reflection.Assembly.GetEntryAssembly()?.GetName().Name} {System.Reflection.Assembly.GetEntryAssembly()?.GetName().Version}; {RuntimeInformation.OSDescription}; {RuntimeInformation.FrameworkDescription}");
        text.AppendLine($"IPv6 on this PC: {NetEnvironment.Ipv6Summary()}");
        if (failure is NetworkFailure f)
            text.AppendLine($"failed at: {NetText.StageName(f.Stage)} ({(f.Hop == 0 ? "" : "on the host a redirect led to, ")}host {f.Host})");
        text.AppendLine("proxy in use:");
        if (hosts.Length == 0) text.AppendLine("  (no request was made)");
        foreach (var uri in hosts) text.AppendLine($"  {uri.Scheme}://{uri.Host}{(uri.IsDefaultPort ? "" : ":" + uri.Port)}/ : {NetEnvironment.DescribeProxy(uri)}");
        text.AppendLine("steps:");
        foreach (var line in Events) text.AppendLine("  " + line);
        if (failure != null)
        {
            text.AppendLine("error:");
            for (var e = failure; e != null; e = e.InnerException)
                text.AppendLine($"  {e.GetType().Name}: {e.Message}");
            if (failure is NetworkFailure { Also: { Count: > 0 } also })
                foreach (var other in also)
                {
                    text.AppendLine("also failed:");
                    for (Exception? e = other; e != null; e = e.InnerException)
                        text.AppendLine($"  {e.GetType().Name}: {e.Message}");
                }
        }
        return text.ToString();
    }
}

static class NetEnvironment
{
    /// "none (direct)" or the proxy's address (host and port only) that the system settings give for `uri`.
    public static string DescribeProxy(Uri uri)
    {
        try
        {
            var proxy = HttpClient.DefaultProxy;
            if (proxy == null || proxy.IsBypassed(uri)) return "none (direct connection)";
            var via = proxy.GetProxy(uri);
            return via == null || via == uri ? "none (direct connection)" : $"{via.Scheme}://{via.Host}:{via.Port} (from the system settings)";
        }
        catch (Exception ex) { return $"unknown ({ex.GetType().Name})"; }
    }

    /// Whether this PC has an IPv6 address that could reach the internet: no address of the kind is why a name's IPv6 addresses cannot work.
    public static string Ipv6Summary()
    {
        try
        {
            if (!Socket.OSSupportsIPv6) return "turned off in Windows";
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType is NetworkInterfaceType.Loopback or NetworkInterfaceType.Tunnel) continue;
                foreach (var unicast in ni.GetIPProperties().UnicastAddresses)
                {
                    var a = unicast.Address;
                    if (a.AddressFamily == AddressFamily.InterNetworkV6 && !a.IsIPv6LinkLocal && !a.IsIPv6SiteLocal && !a.IsIPv6Teredo && !a.IsIPv6UniqueLocal && !IPAddress.IsLoopback(a))
                        return "on, with a global address (names may answer with IPv6 addresses, which have to be routed)";
                }
            }
            return "on, but no global address (only a local one: IPv6 addresses of a name cannot be reached from here)";
        }
        catch (Exception ex) when (ex is NetworkInformationException or PlatformNotSupportedException) { return "unknown"; }
    }
}

/// The connecting went wrong; thrown by the connect callback (and wrapped by the handler in an HttpRequestException).
sealed class ConnectFailure(NetStage stage, string host, int port, string reason, Exception? inner = null)
    : IOException($"{(stage == NetStage.Lookup ? "DNS lookup of" : "connecting to")} {host}{(stage == NetStage.Lookup ? "" : ":" + port)} failed: {reason}", inner)
{
    public NetStage Stage { get; } = stage;
    public string Host { get; } = host;
    public int Port { get; } = port;
    public string Reason { get; } = reason;
}

/// A connected socket and what it took.
sealed record ConnectedSocket(Socket Socket, IPAddress Address, double Milliseconds);

delegate Task<Socket> ConnectOne(IPAddress address, int port, CancellationToken ct);

static class HappyEyeballs
{
    /// How long the first attempt has the field to itself before the next one (the other family) starts (RFC 8305: 250 ms).
    public static TimeSpan AttemptDelay { get; set; } = TimeSpan.FromMilliseconds(250);
    /// How long all the attempts together get.
    public static TimeSpan ConnectBudget { get; set; } = TimeSpan.FromSeconds(15);
    /// How long the name lookup gets.
    public static TimeSpan DnsBudget { get; set; } = TimeSpan.FromSeconds(10);

    /// The order to try: the family the lookup listed first, then the other, taking turns (so that a second address of the first family
    /// comes after the first one of the other).
    public static List<IPAddress> Order(IEnumerable<IPAddress> addresses, bool allowIPv6)
    {
        var usable = new List<IPAddress>();
        foreach (var a in addresses)
        {
            var address = a.IsIPv4MappedToIPv6 ? a.MapToIPv4() : a;
            if (address.AddressFamily == AddressFamily.InterNetworkV6 && !allowIPv6) continue;
            if (address.AddressFamily is AddressFamily.InterNetwork or AddressFamily.InterNetworkV6 && !usable.Contains(address)) usable.Add(address);
        }
        if (usable.Count == 0) return usable;
        var firstFamily = usable[0].AddressFamily;
        var same = usable.Where(a => a.AddressFamily == firstFamily).ToList();
        var other = usable.Where(a => a.AddressFamily != firstFamily).ToList();
        var ordered = new List<IPAddress>(usable.Count);
        for (var i = 0; i < Math.Max(same.Count, other.Count); i++)
        {
            if (i < same.Count) ordered.Add(same[i]);
            if (i < other.Count) ordered.Add(other[i]);
        }
        return ordered;
    }

    public static string Describe(IPAddress a) => $"{a} ({(a.AddressFamily == AddressFamily.InterNetworkV6 ? "IPv6" : "IPv4")})";

    static async Task<Socket> ConnectSocketAsync(IPAddress address, int port, CancellationToken ct)
    {
        var socket = new Socket(address.AddressFamily, SocketType.Stream, ProtocolType.Tcp) { NoDelay = true };
        try
        {
            await socket.ConnectAsync(new IPEndPoint(address, port), ct).ConfigureAwait(false);
            return socket;
        }
        catch
        {
            socket.Dispose();
            throw;
        }
    }

    sealed class Attempt(IPAddress address, Task<Socket> task, long startedMs)
    {
        public IPAddress Address { get; } = address;
        public Task<Socket> Task { get; } = task;
        public long StartedMs { get; } = startedMs;
    }

    static string Why(Exception ex, TimeSpan budget) => ex switch
    {
        SocketException { SocketErrorCode: SocketError.ConnectionRefused } => "refused",
        SocketException { SocketErrorCode: SocketError.TimedOut } => "timed out",
        SocketException { SocketErrorCode: SocketError.NetworkUnreachable } => "network unreachable",
        SocketException { SocketErrorCode: SocketError.HostUnreachable } => "host unreachable",
        SocketException { SocketErrorCode: SocketError.AddressFamilyNotSupported } => "not supported",
        SocketException se => $"{se.SocketErrorCode} ({se.NativeErrorCode})",
        OperationCanceledException => $"no answer in {budget.TotalSeconds:0.#} s",
        _ => ex.Message,
    };

    /// Connects to the first address that answers. Throws ConnectFailure (Stage Connect) when none does within the budget, and
    /// OperationCanceledException when `ct` is cancelled. `connect` is for tests (a connection that hangs, say).
    public static async Task<ConnectedSocket> ConnectAsync(string host, IReadOnlyList<IPAddress> addresses, int port, CancellationToken ct,
        NetOperation? op = null, TimeSpan? attemptDelay = null, TimeSpan? budget = null, ConnectOne? connect = null, bool? allowIPv6 = null)
    {
        var delay = attemptDelay ?? AttemptDelay;
        var limit = budget ?? ConnectBudget;
        connect ??= ConnectSocketAsync;
        var order = Order(addresses, allowIPv6 ?? Socket.OSSupportsIPv6);
        if (order.Count == 0)
            throw new ConnectFailure(NetStage.Connect, host, port, "the name has no address this PC can use");
        op?.Log($"connect {host}:{port}: {order.Count} address{(order.Count == 1 ? "" : "es")} in this order: {string.Join(", ", order.Select(Describe))}; the next one starts after {delay.TotalMilliseconds:0} ms if nothing has connected");

        using var all = CancellationTokenSource.CreateLinkedTokenSource(ct);
        all.CancelAfter(limit);
        var clock = Stopwatch.StartNew();
        var running = new List<Attempt>();
        var failures = new List<string>();
        var next = 0;
        var startNext = false;
        Task? timer = null;
        ConnectedSocket? winner = null;
        try
        {
            while (winner == null && !all.IsCancellationRequested)
            {
                // Nothing running, an attempt that failed, or the delay over with nothing connected: the next address goes.
                if (next < order.Count && (running.Count == 0 || startNext || timer is { IsCompleted: true }))
                {
                    startNext = false;
                    var address = order[next++];
                    var started = clock.ElapsedMilliseconds;
                    op?.Log($"connect {Describe(address)} port {port}: started at {started} ms");
                    var token = all.Token;
                    running.Add(new Attempt(address, Task.Run(() => connect(address, port, token)), started));
                    timer = next < order.Count ? Task.Delay(delay, all.Token) : null;
                }
                if (running.Count == 0) break;
                var waiting = new List<Task>(running.Select(a => (Task)a.Task));
                if (timer != null) waiting.Add(timer);
                var done = await Task.WhenAny(waiting).ConfigureAwait(false);
                if (done == timer) continue;
                var attempt = running.First(a => a.Task == done);
                running.Remove(attempt);
                var took = clock.ElapsedMilliseconds - attempt.StartedMs;
                if (attempt.Task.IsCompletedSuccessfully)
                {
                    winner = new ConnectedSocket(attempt.Task.Result, attempt.Address, took);
                    op?.Log($"connect {Describe(attempt.Address)} port {port}: connected in {took} ms");
                }
                else
                {
                    var why = Why(attempt.Task.Exception?.GetBaseException() ?? new OperationCanceledException(), limit);
                    failures.Add($"{Describe(attempt.Address)}: {why}");
                    op?.Log($"connect {Describe(attempt.Address)} port {port}: failed after {took} ms: {why}");
                    startNext = true;
                }
            }
        }
        finally
        {
            // The attempts that lost are cancelled, and a socket one of them connects in the meantime is closed.
            all.Cancel();
            foreach (var loser in running)
            {
                op?.Log($"connect {Describe(loser.Address)} port {port}: {(winner != null ? "not needed, dropped" : "no answer, given up")}");
                _ = loser.Task.ContinueWith(t => { if (t.IsCompletedSuccessfully) t.Result.Dispose(); else _ = t.Exception; }, TaskScheduler.Default);
            }
        }

        if (winner != null) return winner;
        ct.ThrowIfCancellationRequested();
        var parts = failures
            .Concat(running.Select(a => $"{Describe(a.Address)}: no answer in {limit.TotalSeconds:0.#} s"))
            .Concat(order.Skip(next).Select(a => $"{Describe(a)}: not tried"));
        throw new ConnectFailure(NetStage.Connect, host, port, string.Join("; ", parts));
    }

    /// The name's addresses (a literal address is its own).
    public static async Task<IPAddress[]> ResolveAsync(string host, int port, NetOperation? op, CancellationToken ct,
        TimeSpan? budget = null, Func<string, CancellationToken, Task<IPAddress[]>>? resolver = null)
    {
        if (IPAddress.TryParse(host, out var literal))
        {
            op?.Log($"{host} is an address, no lookup");
            return [literal];
        }
        var limit = budget ?? DnsBudget;
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        cts.CancelAfter(limit);
        var clock = Stopwatch.StartNew();
        try
        {
            var found = await (resolver ?? ((name, token) => Dns.GetHostAddressesAsync(name, token))).Invoke(host, cts.Token).ConfigureAwait(false);
            op?.Log($"DNS {host}: {found.Length} address{(found.Length == 1 ? "" : "es")} in {clock.ElapsedMilliseconds} ms: {string.Join(", ", found.Select(Describe))}");
            if (found.Length == 0) throw new ConnectFailure(NetStage.Lookup, host, port, "the name has no addresses");
            return found;
        }
        catch (SocketException ex)
        {
            var why = ex.SocketErrorCode switch
            {
                SocketError.HostNotFound => "no such name",
                SocketError.TryAgain => "the DNS server didn't answer",
                SocketError.NoData => "the name has no addresses",
                _ => $"{ex.SocketErrorCode}",
            };
            op?.Log($"DNS {host}: failed after {clock.ElapsedMilliseconds} ms: {why} ({ex.SocketErrorCode}, {ex.NativeErrorCode})");
            throw new ConnectFailure(NetStage.Lookup, host, port, why, ex);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            op?.Log($"DNS {host}: no answer in {limit.TotalSeconds:0.#} s");
            throw new ConnectFailure(NetStage.Lookup, host, port, $"the DNS server didn't answer in {limit.TotalSeconds:0.#} s");
        }
    }

    /// For tests only: a lookup and a connect that stand in for the real ones in the connect callback.
    internal static Func<string, CancellationToken, Task<IPAddress[]>>? ResolverOverride { get; set; }
    internal static ConnectOne? ConnectorOverride { get; set; }

    /// SocketsHttpHandler.ConnectCallback: resolve, connect (the families racing), hand over the stream. The handler does the
    /// secure handshake and the proxy's CONNECT on it itself.
    internal static async ValueTask<Stream> ConnectCallback(SocketsHttpConnectionContext context, CancellationToken ct)
    {
        NetOperation? op = null;
        context.InitialRequestMessage?.Options.TryGetValue(NetClient.OperationKey, out op);
        var host = context.DnsEndPoint.Host;
        var port = context.DnsEndPoint.Port;
        if (op != null) op.Stage = NetStage.Lookup;
        var addresses = await ResolveAsync(host, port, op, ct, resolver: ResolverOverride).ConfigureAwait(false);
        if (op != null) op.Stage = NetStage.Connect;
        var connected = await ConnectAsync(host, addresses, port, ct, op, connect: ConnectorOverride, allowIPv6: ConnectorOverride != null ? true : null).ConfigureAwait(false);
        if (op != null) op.Stage = NetStage.Secure;
        return new NetworkStream(connected.Socket, ownsSocket: true);
    }
}

/// The settings of one GetAsync.
sealed record NetRequest
{
    /// Another client (a test's), in place of the shared one.
    public HttpClient? Client { get; init; }
    /// How long a request has to get its answer (the headers), counting the lookup, the connection and the handshake.
    public TimeSpan AnswerTimeout { get; init; } = TimeSpan.FromSeconds(30);
    public int MaxRedirects { get; init; } = 5;
    /// Plain http (tests with a local server). A player's run never sets it.
    public bool AllowPlainHttp { get; init; }
    /// Headers of the first request.
    public Action<HttpRequestMessage>? Configure { get; init; }
}

/// A failure of the network, in the words the player sees (Plain) and the rest for "Copy details" (Report).
class NetworkFailure : HttpRequestException
{
    public NetworkFailure(string plain, NetStage stage, NetOperation operation, Exception? inner = null, HttpStatusCode? status = null, bool retriable = true)
        : base(plain, inner, status)
    {
        Stage = stage;
        Operation = operation;
        Host = operation.Host;
        Hop = operation.DownloadHop;
        Retriable = retriable;
    }

    public string Plain => Message;
    public NetStage Stage { get; }
    public string Host { get; init; }
    /// 0 for the address that was asked for; more for a host a redirect led to.
    public int Hop { get; init; }
    public NetOperation Operation { get; }
    /// False when trying again, or another way, can't help (the release isn't there; a redirect that isn't safe).
    public bool Retriable { get; }
    /// GitHub's API turned the request away because of its limit for this internet address.
    public bool RateLimited { get; init; }
    public DateTimeOffset? RetryAt { get; init; }
    /// Other failures of the same job (the direct route failed, then the API did too).
    public List<Exception> Also { get; init; } = [];

    public string Report() => Operation.Report(this);
}

static class NetText
{
    public static string StageName(NetStage stage) => stage switch
    {
        NetStage.Lookup => "looking up the name (DNS)",
        NetStage.Connect => "connecting",
        NetStage.Secure => "the secure handshake",
        NetStage.Answer => "waiting for the answer",
        NetStage.Status => "the server's answer",
        NetStage.Read => "reading the file",
        _ => "following a redirect",
    };

    /// "github.com", or "the download host release-assets.githubusercontent.com" for a host a redirect led to.
    public static string Where(string host, int hop, bool proxy = false) => proxy ? $"the proxy {host}" : hop == 0 ? host : $"the download host {host}";

    public static string Failed(NetStage stage, string host, int hop, string reason, bool proxy = false)
    {
        var where = Where(host, hop, proxy);
        var sentOn = hop == 0 || proxy ? "" : " (GitHub itself answered and sent the request there)";
        return stage switch
        {
            NetStage.Lookup => $"The DNS lookup for {where} failed ({reason}): Windows couldn't turn that name into an address{sentOn}. Check the internet connection, and any VPN, DNS filter or firewall.",
            NetStage.Connect => $"Couldn't connect to {where}{sentOn}: {reason}. Something between this PC and it (a firewall, a VPN or proxy, the network or the provider) isn't letting the connection through.",
            NetStage.Secure => $"The secure connection to {where} couldn't be set up ({reason}){sentOn}. A VPN, proxy, antivirus or firewall that inspects secure traffic, or a wrong date or time on this PC, is the usual cause.",
            NetStage.Answer => $"{Capital(where)} {reason}{sentOn}. A firewall, a proxy or a network sign-in page can hold the request back.",
            NetStage.Read => $"The connection to {where} {reason}{sentOn}.",
            NetStage.Redirect => $"GitHub's answer sent the request on in a way the program doesn't follow ({reason}).",
            _ => $"{Capital(where)} answered {reason}.",
        };
    }

    static string Capital(string text) => text.Length == 0 ? text : char.ToUpperInvariant(text[0]) + text[1..];
}

static class NetClient
{
    public static readonly HttpRequestOptionsKey<NetOperation> OperationKey = new("mcla.net.operation");

    static readonly Lazy<HttpClient> shared = new(() => Create());

    /// The client of the whole program: a connection pool is worth sharing.
    public static HttpClient Shared => shared.Value;

    /// A handler that connects the way HappyEyeballs says. `proxy` is for tests; without it the system's proxy settings are used.
    public static SocketsHttpHandler CreateHandler(IWebProxy? proxy = null)
    {
        var handler = new SocketsHttpHandler
        {
            // GetAsync follows the redirects itself (https only, and it knows which host a failure was on).
            AllowAutoRedirect = false,
            ConnectCallback = HappyEyeballs.ConnectCallback,
            ConnectTimeout = HappyEyeballs.DnsBudget + HappyEyeballs.ConnectBudget + TimeSpan.FromSeconds(5),
            PooledConnectionLifetime = TimeSpan.FromMinutes(5),
            // A proxy that asks for a login gets the Windows one, as a browser's does.
            DefaultProxyCredentials = CredentialCache.DefaultCredentials,
        };
        if (proxy != null) { handler.UseProxy = true; handler.Proxy = proxy; }
        return handler;
    }

    /// A client with no overall timeout (a download is as long as it is; GetAsync and the readers have their own).
    public static HttpClient Create(HttpMessageHandler? handler = null)
    {
        var client = new HttpClient(handler ?? CreateHandler(), disposeHandler: handler == null) { Timeout = Timeout.InfiniteTimeSpan };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("MclaLauncher/1.0");
        return client;
    }

    static bool IsRedirect(HttpStatusCode code) => code is HttpStatusCode.MovedPermanently or HttpStatusCode.Found or HttpStatusCode.SeeOther
        or HttpStatusCode.TemporaryRedirect or HttpStatusCode.PermanentRedirect;

    /// "https://host/path" without a query: the query of a download link is a signed token.
    static string ForLog(Uri uri) => $"{uri.Scheme}://{uri.Authority}{uri.AbsolutePath}{(uri.Query.Length > 0 ? "?..." : "")}";

    /// GET, following redirects (https only; at most `MaxRedirects`). Returns the answer of the last address whatever its status;
    /// the caller reads the body, and checks the status (StatusFailure). Throws NetworkFailure when no answer came.
    public static async Task<HttpResponseMessage> GetAsync(string url, NetOperation op, NetRequest? options, CancellationToken ct)
    {
        options ??= new NetRequest();
        var client = options.Client ?? Shared;
        if (!Uri.TryCreate(url, UriKind.Absolute, out var current))
            throw new NetworkFailure($"The address {url} isn't usable.", NetStage.Redirect, op, retriable: false);
        for (var hop = 0; ; hop++)
        {
            if (hop > options.MaxRedirects)
                throw new NetworkFailure(NetText.Failed(NetStage.Redirect, current.Host, hop, $"more than {options.MaxRedirects} redirects"), NetStage.Redirect, op, retriable: false);
            var ok = current.Scheme == Uri.UriSchemeHttps || (options.AllowPlainHttp && current.Scheme == Uri.UriSchemeHttp);
            if (!ok)
            {
                op.BeginHop(hop, current);
                op.Log($"refused: {ForLog(current)} is not https");
                throw new NetworkFailure(NetText.Failed(NetStage.Redirect, current.Host, hop, "it leads to an address that isn't https"), NetStage.Redirect, op, retriable: false);
            }
            op.BeginHop(hop, current);
            op.Log($"GET {ForLog(current)}{(hop > 0 ? $" (redirect {hop})" : "")}");
            using var request = new HttpRequestMessage(HttpMethod.Get, current);
            if (hop == 0) options.Configure?.Invoke(request);
            request.Options.Set(OperationKey, op);
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(options.AnswerTimeout);
            HttpResponseMessage response;
            try
            {
                response = await client.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, timeout.Token).ConfigureAwait(false);
            }
            catch (Exception ex) when (ex is not OperationCanceledException || !ct.IsCancellationRequested)
            {
                throw Classify(ex, op, timedOut: timeout.IsCancellationRequested && !ct.IsCancellationRequested, options.AnswerTimeout);
            }
            op.Log($"answer {(int)response.StatusCode} {response.ReasonPhrase} from {current.Host}");
            if (!IsRedirect(response.StatusCode)) return response;
            var location = response.Headers.Location;
            response.Dispose();
            if (location == null)
                throw new NetworkFailure(NetText.Failed(NetStage.Redirect, current.Host, hop, "it names no address to go to"), NetStage.Redirect, op, retriable: false);
            current = location.IsAbsoluteUri ? location : new Uri(current, location);
            op.Log($"redirected to {current.Host}");
        }
    }

    static IEnumerable<Exception> Chain(Exception ex)
    {
        for (Exception? e = ex; e != null; e = e.InnerException) yield return e;
    }

    /// What went wrong while a request was being sent, as a NetworkFailure naming the stage.
    static NetworkFailure Classify(Exception ex, NetOperation op, bool timedOut, TimeSpan limit)
    {
        var host = op.Host;
        var hop = op.DownloadHop;
        op.Log($"failed: {string.Join(" <- ", Chain(ex).Select(e => $"{e.GetType().Name}: {e.Message}"))}");
        var chain = Chain(ex).ToList();
        if (chain.OfType<ConnectFailure>().FirstOrDefault() is { } connect)
            return new NetworkFailure(NetText.Failed(connect.Stage, connect.Host, hop, connect.Reason, proxy: !string.Equals(connect.Host, host, StringComparison.OrdinalIgnoreCase)), connect.Stage, op, ex);
        if (timedOut || ex is OperationCanceledException or TimeoutException)
        {
            var reason = op.Stage switch
            {
                NetStage.Lookup => $"didn't answer within {limit.TotalSeconds:0.#} s",
                NetStage.Connect => $"didn't answer within {limit.TotalSeconds:0.#} s",
                _ => $"connected but didn't answer within {limit.TotalSeconds:0.#} s",
            };
            return new NetworkFailure(NetText.Failed(NetStage.Answer, host, hop, reason), NetStage.Answer, op, ex);
        }
        if (chain.OfType<AuthenticationException>().FirstOrDefault() is { } auth)
        {
            var detail = auth.InnerException != null && auth.InnerException.Message != auth.Message ? $"{auth.Message} {auth.InnerException.Message}" : auth.Message;
            return new NetworkFailure(NetText.Failed(NetStage.Secure, host, hop, detail.TrimEnd('.')), NetStage.Secure, op, ex);
        }
        if (ex is HttpRequestException { HttpRequestError: HttpRequestError.SecureConnectionError })
            return new NetworkFailure(NetText.Failed(NetStage.Secure, host, hop, "the handshake failed"), NetStage.Secure, op, ex);
        if (ex is HttpRequestException { HttpRequestError: HttpRequestError.ProxyTunnelError })
            return new NetworkFailure(NetText.Failed(NetStage.Connect, host, hop, "the proxy didn't open the connection (it may need a sign-in)"), NetStage.Connect, op, ex);
        if (chain.OfType<SocketException>().FirstOrDefault() is { } socket)
            return new NetworkFailure(NetText.Failed(op.Stage <= NetStage.Connect ? NetStage.Connect : NetStage.Answer, host, hop,
                op.Stage <= NetStage.Connect ? socket.SocketErrorCode.ToString() : $"closed the connection ({socket.SocketErrorCode})"),
                op.Stage <= NetStage.Connect ? NetStage.Connect : NetStage.Answer, op, ex);
        var stage = op.Stage <= NetStage.Connect ? NetStage.Connect : NetStage.Answer;
        return new NetworkFailure(NetText.Failed(stage, host, hop, stage == NetStage.Connect ? ex.Message : $"closed the connection without a proper answer ({chain.Last().Message.TrimEnd('.')})"), stage, op, ex);
    }

    /// The failure for an answer that isn't the file: 404 (not retriable), GitHub's API limit, and the rest.
    public static NetworkFailure StatusFailure(HttpResponseMessage response, NetOperation op)
    {
        var code = response.StatusCode;
        var host = op.Host;
        var hop = op.DownloadHop;
        string? Header(string name) => response.Headers.TryGetValues(name, out var values) ? values.FirstOrDefault() : null;
        if (code is HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests && (Header("X-RateLimit-Remaining") == "0" || Header("Retry-After") != null))
        {
            DateTimeOffset? at = long.TryParse(Header("X-RateLimit-Reset"), out var epoch) ? DateTimeOffset.FromUnixTimeSeconds(epoch) : null;
            if (at == null && int.TryParse(Header("Retry-After"), out var seconds)) at = DateTimeOffset.Now.AddSeconds(seconds);
            var when = at is { } t ? $" It lets this address ask again at {t.ToLocalTime():HH:mm}." : "";
            op.Log($"{(int)code}: GitHub's rate limit for this address is used up{(at is { } r ? $" until {r.ToLocalTime():HH:mm:ss}" : "")}");
            return new NetworkFailure($"{Capital(host)} refused the request because this internet address has used up its limit of requests (GitHub allows 60 an hour without signing in).{when}",
                NetStage.Status, op, status: code) { RateLimited = true, RetryAt = at };
        }
        var proxyAsk = code == HttpStatusCode.ProxyAuthenticationRequired;
        var text = proxyAsk
            ? "the proxy asks for a sign-in (407) that Windows didn't give it"
            : $"{(int)code} {response.ReasonPhrase}".Trim();
        op.Log($"status {text} from {host}");
        return new NetworkFailure(NetText.Failed(NetStage.Status, host, hop, text) + (code == HttpStatusCode.NotFound ? "" : " Try again in a while."),
            NetStage.Status, op, status: code, retriable: code != HttpStatusCode.NotFound);

        static string Capital(string s) => s.Length == 0 ? s : char.ToUpperInvariant(s[0]) + s[1..];
    }

    /// A failure while the body was being read (the network, not the disk).
    public static NetworkFailure ReadFailure(Exception ex, NetOperation op, bool stalled, TimeSpan stall)
    {
        op.Stage = NetStage.Read;
        var reason = stalled ? $"stopped sending data for {stall.TotalSeconds:0.#} s" : $"was lost while the file was being read ({Chain(ex).Last().Message.TrimEnd('.')})";
        op.Log($"read failed: {(stalled ? "no data for " + stall.TotalSeconds.ToString("0.#") + " s" : string.Join(" <- ", Chain(ex).Select(e => e.Message)))}");
        return new NetworkFailure(NetText.Failed(NetStage.Read, op.Host, op.DownloadHop, reason), NetStage.Read, op, ex);
    }

    /// The body as text (UTF-8), at most `limit` bytes, with `stall` as the longest wait for the next bytes.
    public static async Task<string> ReadTextAsync(HttpResponseMessage response, NetOperation op, TimeSpan stall, int limit, CancellationToken ct)
    {
        if (response.Content.Headers.ContentLength is long length && length > limit)
            throw new NetworkFailure(NetText.Failed(NetStage.Read, op.Host, op.DownloadHop, $"the answer is {length:N0} bytes, which is more than is expected here"), NetStage.Read, op, retriable: false);
        using var buffer = new MemoryStream();
        using var stalled = CancellationTokenSource.CreateLinkedTokenSource(ct);
        try
        {
            stalled.CancelAfter(stall);
            await using var input = await response.Content.ReadAsStreamAsync(stalled.Token).ConfigureAwait(false);
            var chunk = new byte[16 * 1024];
            while (true)
            {
                stalled.CancelAfter(stall);
                var read = await input.ReadAsync(chunk, stalled.Token).ConfigureAwait(false);
                if (read <= 0) break;
                if (buffer.Length + read > limit)
                    throw new NetworkFailure(NetText.Failed(NetStage.Read, op.Host, op.DownloadHop, "the answer is longer than is expected here"), NetStage.Read, op, retriable: false);
                buffer.Write(chunk, 0, read);
            }
        }
        catch (Exception ex) when (ex is not NetworkFailure && (ex is not OperationCanceledException || !ct.IsCancellationRequested))
        {
            throw ReadFailure(ex, op, stalled.IsCancellationRequested, stall);
        }
        op.Log($"read {buffer.Length:N0} bytes");
        var bytes = buffer.GetBuffer().AsSpan(0, (int)buffer.Length);
        if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) bytes = bytes[3..];
        return Encoding.UTF8.GetString(bytes);
    }
}

/// Where release.json is on the web, and GitHub's API for the same repository.
sealed record ReleaseEndpoints(string Web, string Api, string Repository, bool AllowPlainHttp = false)
{
    public static ReleaseEndpoints GitHub(string repository) => new("https://github.com", "https://api.github.com", repository);

    public string DirectUrl => $"{Web}/{Repository}/releases/latest/download/release.json";
    public string ApiUrl => $"{Api}/repos/{Repository}/releases/latest";
}

/// What a fetch of release.json took, for the log of the setup.
enum ReleaseRoute { Direct, SecondTry, Api }

static class ReleaseFetch
{
    /// Pauses and time limits of the routes (tests make them short).
    public sealed record Timing
    {
        public TimeSpan FirstAnswer { get; init; } = TimeSpan.FromSeconds(30);
        public TimeSpan LaterAnswer { get; init; } = TimeSpan.FromSeconds(20);
        public TimeSpan RetryPause { get; init; } = TimeSpan.FromSeconds(2);
        public TimeSpan Stall { get; init; } = TimeSpan.FromSeconds(30);
        public int MaxBytes { get; init; } = 2 << 20;
        public HttpClient? Client { get; init; }
    }

    /// release.json of the repository's latest release: its direct address; once more after a short pause if that failed; then the
    /// API's way (the release's asset list, and the asset's own address). Returns the text and the route that worked. A 404
    /// from the direct address (no release yet) is final. Throws NetworkFailure (with the other routes' failures in Also).
    public static async Task<(string Json, ReleaseRoute Route)> FetchAsync(ReleaseEndpoints endpoints, NetOperation op, Action<string>? status, CancellationToken ct, Timing? timing = null)
    {
        timing ??= new Timing();
        NetworkFailure? direct = null;
        for (var attempt = 1; attempt <= 2; attempt++)
        {
            status?.Invoke(attempt == 1 ? "Looking up the latest release…" : "Trying again…");
            op.Log(attempt == 1 ? "route 1: release.json at its address" : "route 1 again, after a pause");
            try
            {
                var json = await GetTextAsync(endpoints.DirectUrl, op, endpoints, attempt == 1 ? timing.FirstAnswer : timing.LaterAnswer, timing, null, ct).ConfigureAwait(false);
                return (json, attempt == 1 ? ReleaseRoute.Direct : ReleaseRoute.SecondTry);
            }
            catch (NetworkFailure failure)
            {
                op.Log($"route 1, try {attempt}, failed: {failure.Plain}");
                if (!failure.Retriable) throw;
                direct = failure;
            }
            if (attempt == 1) await Task.Delay(timing.RetryPause, ct).ConfigureAwait(false);
        }

        status?.Invoke("Asking GitHub's API instead…");
        op.Log("route 2: GitHub's API (the release's list of files)");
        try
        {
            return (await ViaApiAsync(endpoints, op, timing, ct).ConfigureAwait(false), ReleaseRoute.Api);
        }
        catch (NetworkFailure api)
        {
            op.Log($"route 2 failed: {api.Plain}");
            var note = api.RateLimited
                ? $" Asking GitHub's API instead didn't work either: its limit of requests for this address is used up{(api.RetryAt is { } at ? $" until {at.ToLocalTime():HH:mm}" : "")}."
                : $" Asking GitHub's API instead didn't work either ({NetText.StageName(api.Stage)}, {api.Host}).";
            throw new NetworkFailure(direct!.Plain + note, direct.Stage, op, direct, direct.StatusCode, retriable: false) { Host = direct.Host, Hop = direct.Hop, Also = [api] };
        }
    }

    static async Task<string> GetTextAsync(string url, NetOperation op, ReleaseEndpoints endpoints, TimeSpan answer, Timing timing, Action<HttpRequestMessage>? configure, CancellationToken ct)
    {
        var request = new NetRequest { Client = timing.Client, AnswerTimeout = answer, AllowPlainHttp = endpoints.AllowPlainHttp, Configure = configure };
        using var response = await NetClient.GetAsync(url, op, request, ct).ConfigureAwait(false);
        if (!response.IsSuccessStatusCode) throw NetClient.StatusFailure(response, op);
        return await NetClient.ReadTextAsync(response, op, timing.Stall, timing.MaxBytes, ct).ConfigureAwait(false);
    }

    internal static async Task<string> ViaApiAsync(ReleaseEndpoints endpoints, NetOperation op, Timing timing, CancellationToken ct)
    {
        var text = await GetTextAsync(endpoints.ApiUrl, op, endpoints, timing.LaterAnswer, timing, request =>
        {
            request.Headers.Accept.ParseAdd("application/vnd.github+json");
            request.Headers.Add("X-GitHub-Api-Version", "2022-11-28");
        }, ct).ConfigureAwait(false);
        string? assetUrl = null;
        try
        {
            using var doc = JsonDocument.Parse(text);
            if (doc.RootElement.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array)
                foreach (var asset in assets.EnumerateArray())
                    if (asset.TryGetProperty("name", out var name) && name.GetString() == "release.json" && asset.TryGetProperty("browser_download_url", out var link))
                    {
                        assetUrl = link.GetString();
                        break;
                    }
        }
        catch (JsonException ex)
        {
            throw new NetworkFailure("GitHub's API answered with something that isn't a list of the release's files.", NetStage.Status, op, ex, retriable: false);
        }
        // The API names where the file is; it has to be this repository's release on GitHub's own site, and https.
        var expected = $"{endpoints.Web}/{endpoints.Repository}/releases/download/";
        if (assetUrl == null || !assetUrl.StartsWith(expected, StringComparison.OrdinalIgnoreCase))
            throw new NetworkFailure(assetUrl == null ? "The latest release has no release.json file." : "GitHub's API named a download address that isn't this repository's release; it was not used.",
                NetStage.Status, op, status: HttpStatusCode.NotFound, retriable: false);
        op.Log($"the API names release.json at {assetUrl}");
        return await GetTextAsync(assetUrl, op, endpoints, timing.LaterAnswer, timing, null, ct).ConfigureAwait(false);
    }
}

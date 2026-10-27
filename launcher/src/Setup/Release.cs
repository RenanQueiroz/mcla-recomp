// release.json: what a release of the fork holds (scripts/ci/assemble-release.py writes it), and where the
// wizard gets it. The real source is the fork's latest published release on GitHub (fetched by Net.cs: both IP
// families raced, one more try, GitHub's API as the second route); a folder with release.json and the assets
// stands in for it: either the player's own downloads ("Use files I downloaded", IsOffline) or a development
// override (a folder, or a file:// URL), which the window announces.
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MclaLauncher.Setup;

/// A file of the release. SourceCommit: for the launcher, the last commit that changed its source (what its
/// source-commit.txt says), which is how a running launcher knows that a release has a newer one.
sealed record ReleaseAsset(string Asset, string Sha256, long Size, string? SourceCommit = null);

sealed record ReleaseTool(string Version, string Url, string Sha256, long Size);

sealed record ReleaseGame(string Title, string TitleId, string XexVersion, List<GameFileInfo> Files)
{
    public GameFingerprint ToFingerprint() => new(Title, TitleId, XexVersion, Files);
}

sealed record ReleaseBundle(string Asset, string Sha256, long Size, string SdkCommit, string Toolchain, string LlvmMingw, string PatchesDigest);

sealed record ReleaseShaderPack(string Path, string Sha256, long Size);

sealed record ReleaseInfo
{
    public int Schema { get; init; }
    public string Tag { get; init; } = "";
    public string Commit { get; init; } = "";
    public string Repository { get; init; } = "";
    public ReleaseAsset? Launcher { get; init; }
    /// MCLA-Setup.exe: the installer that also replaces the launcher (SelfUpdate, LauncherUpdate); the launcher's folder is inside it.
    public ReleaseAsset? Installer { get; init; }
    public ReleaseBundle? SdkBundle { get; init; }
    public Dictionary<string, ReleaseTool> Tools { get; init; } = [];
    public ReleaseGame? Game { get; init; }
    public ReleaseShaderPack? ShaderPack { get; init; }

    public static ReleaseInfo Parse(string json)
    {
        ReleaseInfo? info;
        try { info = JsonSerializer.Deserialize<ReleaseInfo>(json, ReleaseJson.Options); }
        catch (JsonException ex)
        {
            throw new SetupException("The release's description (release.json) can't be read, so the setup can't tell what to install. Try again; if it keeps happening, report it.", ex.Message, ex);
        }
        if (info == null) throw new SetupException("The release's description (release.json) is empty.");
        info.Validate();
        return info;
    }

    /// What the wizard needs, checked once: a release without any of it can't be installed from.
    void Validate()
    {
        if (Schema != 1)
            throw new SetupException($"This release was made for a newer launcher (release.json schema {Schema}). Download the latest launcher from the releases page.");
        if (!System.Text.RegularExpressions.Regex.IsMatch(Tag, @"^[A-Za-z0-9][A-Za-z0-9._-]{0,62}\z"))
            throw new SetupException("The release's description has no usable tag name.");
        if (!System.Text.RegularExpressions.Regex.IsMatch(Commit, @"^[0-9a-f]{40}\z"))
            throw new SetupException("The release's description has no full commit hash.");
        if (SdkBundle == null || SdkBundle.Toolchain != "mingw")
            throw new SetupException("This release has no SDK bundle for the llvm-mingw toolchain, which the setup uses.");
        // Names that come from release.json become paths and URLs: plain file names only, https only.
        if (!IsPlainFileName(SdkBundle.Asset) || !IsSha256(SdkBundle.Sha256) || SdkBundle.Size <= 0)
            throw new SetupException("The release's description names the SDK bundle in a way that can't be used.");
        foreach (var name in RequiredTools)
        {
            if (!Tools.TryGetValue(name, out var tool))
                throw new SetupException($"This release doesn't say where to get {name}.");
            if (!System.Text.RegularExpressions.Regex.IsMatch(tool.Version ?? "", @"^[A-Za-z0-9][A-Za-z0-9._+-]{0,40}\z")
                || !IsSha256(tool.Sha256) || tool.Size <= 0
                || !Uri.TryCreate(tool.Url, UriKind.Absolute, out var uri) || uri.Scheme != Uri.UriSchemeHttps
                || !IsPlainFileName(Path.GetFileName(uri.AbsolutePath)))
                throw new SetupException($"The release's description names {name}'s download in a way that can't be used (it has to be an https address of a file).", tool.Url);
        }
        if (Launcher is { } launcher && (!IsPlainFileName(launcher.Asset) || !IsSha256(launcher.Sha256) || launcher.Size <= 0))
            throw new SetupException("The release's description names the launcher in a way that can't be used.");
        if (Installer is { } installer && (!IsPlainFileName(installer.Asset) || !IsSha256(installer.Sha256) || installer.Size <= 0))
            throw new SetupException("The release's description names the installer in a way that can't be used.");
        if (ShaderPack is { } pack
            && (pack.Path.Length == 0 || pack.Path.Contains("..") || pack.Path.Contains(':') || Path.IsPathRooted(pack.Path)
                || pack.Path.StartsWith('/') || pack.Path.StartsWith('\\') || !IsSha256(pack.Sha256)))
            throw new SetupException("The release's description names the shader pack in a way that can't be used.");
        if (Game == null || Game.Files.Count == 0)
            throw new SetupException("This release has no list of the game's files, so it can't check your game. A newer release will have one.");
        foreach (var file in Game.Files)
        {
            // Names are plain file names on the disc; nothing here may reach outside the game's folder.
            if (file.Path.Length == 0 || file.Path.Contains("..") || file.Path.Contains(':') || Path.IsPathRooted(file.Path)
                || file.Path.StartsWith('/') || file.Path.StartsWith('\\') || file.Size < 0 || file.Sha256.Length != 64)
                throw new SetupException($"The release's list of the game's files has a bad entry ({file.Path}).");
        }
        if (Game.Files.All(f => !string.Equals(f.Path, GameFingerprint.XexName, StringComparison.OrdinalIgnoreCase)))
            throw new SetupException("The release's list of the game's files doesn't include default.xex.");
    }

    public static readonly string[] RequiredTools = ["llvm-mingw", "cmake", "ninja", "mingit"];

    static bool IsSha256(string? text) => text is { Length: 64 } && text.All(Uri.IsHexDigit);

    static bool IsPlainFileName(string? name) =>
        !string.IsNullOrEmpty(name) && name.Length <= 200 && name == Path.GetFileName(name) && name != "." && name != ".."
        && !name.Contains(':') && !ToolInstaller.IsOddName(name);
}

static class ReleaseJson
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };
}

/// A failure in the plain words the window shows, and the detail for the log.
class SetupException(string plain, string? detail = null, Exception? inner = null) : Exception(plain, inner)
{
    public string Plain => Message;
    public string? Detail { get; } = detail;
}

/// A file the offline source needs, and whether the folder has it (with the size the release says).
sealed record OfflineFile(string Name, string Url, long Size, bool Present);

/// Where release.json and the assets come from.
sealed class ReleaseSource
{
    /// The fork's repository, as the real source and the clone name it.
    public const string Repository = "RenanQueiroz/mcla-recomp";
    public const string EnvironmentVariable = "MCLA_RELEASE_SOURCE";
    /// How long GitHub has to answer a request, and how long a download may go without a byte.
    public static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan StallTimeout = TimeSpan.FromSeconds(60);

    /// A folder, when the development override or the player's downloaded files are in use; null for the real source.
    public string? LocalFolder { get; }
    /// The folder is the player's own downloads (not the development override): release.json is held to the same rules as
    /// GitHub's, and dev-source.json in it is ignored.
    public bool IsOffline { get; }
    public ReleaseInfo? Release { get; private set; }
    /// Which route release.json came by (null before it is loaded, or for a folder).
    public ReleaseRoute? Route { get; private set; }
    /// What the last fetch of release.json did, for the details of an error.
    public NetOperation? LastOperation { get; private set; }
    /// The git URL of the fork: GitHub's, or the override's (dev-source.json).
    public string GitUrl { get; private set; } = $"https://github.com/{Repository}.git";
    /// Where the web and the API of GitHub are: the real ones, unless a development option says otherwise.
    public ReleaseEndpoints Endpoints { get; private init; } = ReleaseEndpoints.GitHub(Repository);
    public bool IsOverride => LocalFolder != null;
    public string Describe => IsOffline
        ? $"the files the player downloaded: {LocalFolder}"
        : IsOverride
            ? $"a local release source: {LocalFolder}"
            : $"GitHub: {Repository}" + (Endpoints.Web != "https://github.com" ? $" (at {Endpoints.Web}, a development option)" : "")
              + (Route is ReleaseRoute.SecondTry ? " (second try)" : Route is ReleaseRoute.Api ? " (by GitHub's API)" : "");

    /// The release.json address to open in a browser, when the setup can't get it.
    public string ReleaseJsonUrl => Endpoints.DirectUrl;
    /// The page to download the files from.
    public static string ReleasesPage => $"https://github.com/{Repository}/releases/latest";

    /// The override from the command line or the environment: a folder or a file:// URL. Anything else is refused,
    /// so that the override can't quietly point the wizard at a web server.
    public static ReleaseSource FromOverride(string? value)
    {
        var endpoints = DevEndpoints();
        if (string.IsNullOrWhiteSpace(value)) return new ReleaseSource(null, false) { Endpoints = endpoints };
        string folder = value.Trim();
        if (Uri.TryCreate(folder, UriKind.Absolute, out var uri) && uri.IsFile) folder = uri.LocalPath;
        else if (Uri.TryCreate(folder, UriKind.Absolute, out var other) && other.Scheme is "http" or "https")
            throw new SetupException("The development release source has to be a local folder or a file:// URL.");
        folder = Path.GetFullPath(folder);
        if (!Directory.Exists(folder)) throw new SetupException($"The development release source {folder} isn't a folder.");
        return new ReleaseSource(folder, false);
    }

    /// The player's own downloads: a folder with release.json and the files that belong to it (OfflineFiles).
    public static ReleaseSource FromDownloads(string folder)
    {
        folder = Path.GetFullPath(folder.Trim().Trim('"'));
        if (!Directory.Exists(folder)) throw new SetupException($"{folder} isn't a folder.");
        return new ReleaseSource(folder, true);
    }

    /// The development options that move the web or the API of GitHub to a server of the tester's (https, or http on this PC only).
    static ReleaseEndpoints DevEndpoints()
    {
        var web = DevOptions.Current.GitHubWeb;
        var api = DevOptions.Current.GitHubApi;
        var standard = ReleaseEndpoints.GitHub(Repository);
        if (web == null && api == null) return standard;
        static string Checked(string value)
        {
            if (!Uri.TryCreate(value.TrimEnd('/'), UriKind.Absolute, out var uri) || !(uri.Scheme == Uri.UriSchemeHttps || (uri.Scheme == Uri.UriSchemeHttp && uri.IsLoopback)))
                throw new SetupException($"The development address {value} has to be https, or http on this PC (127.0.0.1).");
            return uri.GetLeftPart(UriPartial.Authority);
        }
        var checkedWeb = web != null ? Checked(web) : standard.Web;
        var checkedApi = api != null ? Checked(api) : standard.Api;
        return standard with { Web = checkedWeb, Api = checkedApi, AllowPlainHttp = checkedWeb.StartsWith("http://") || checkedApi.StartsWith("http://") };
    }

    ReleaseSource(string? localFolder, bool offline)
    {
        LocalFolder = localFolder;
        IsOffline = offline;
    }

    /// release.json: from the folder, or from GitHub. `status` is told what is being tried (the page shows it while the
    /// second try and GitHub's API are tried after a failure). Throws SetupException; for a network failure its Detail
    /// is the report for "Copy details" and its InnerException is the NetworkFailure.
    public async Task<ReleaseInfo> LoadAsync(CancellationToken ct, Action<string>? status = null)
    {
        string json;
        if (LocalFolder != null)
        {
            var file = Path.Combine(LocalFolder, "release.json");
            if (!File.Exists(file))
                throw new SetupException(IsOffline
                    ? $"There is no release.json in {LocalFolder}. Download release.json from the release's page in your web browser into that folder."
                    : $"There is no release.json in {LocalFolder}.");
            json = await File.ReadAllTextAsync(file, ct);
            if (json.TrimStart().StartsWith('<'))
                throw new SetupException($"The release.json in {LocalFolder} is a web page, not the file: the browser saved a page (a sign-in page, perhaps). Download the file again: on the release's page, click release.json under Assets.");
            var dev = Path.Combine(LocalFolder, "dev-source.json");
            if (!IsOffline && File.Exists(dev))
            {
                using var doc = JsonDocument.Parse(await File.ReadAllTextAsync(dev, ct));
                if (doc.RootElement.TryGetProperty("gitUrl", out var url) && url.GetString() is { Length: > 0 } git) GitUrl = git;
            }
        }
        else
        {
            var operation = new NetOperation("find the latest release");
            LastOperation = operation;
            try
            {
                (json, var route) = await ReleaseFetch.FetchAsync(Endpoints, operation, status, ct);
                Route = route;
            }
            catch (NetworkFailure failure) when (failure is { StatusCode: System.Net.HttpStatusCode.NotFound, Also.Count: 0 })
            {
                throw new SetupException("There is no release of the game's installer on GitHub yet. Try again later.", failure.Report(), failure);
            }
            catch (NetworkFailure failure)
            {
                throw new SetupException(failure.Plain, failure.Report(), failure);
            }
            // A page where release.json should be: the sign-in page of a network, or a filter's notice.
            if (json.TrimStart().StartsWith('<'))
            {
                var start = json.Trim();
                throw new SetupException("GitHub's address answered with a web page, not release.json. A network sign-in page (a hotel's, a school's) or a filter in between can do that: open the address in a browser to see.",
                                         operation.Report() + "\nfirst bytes: " + start[..Math.Min(200, start.Length)]);
            }
        }
        Release = ReleaseInfo.Parse(json);
        if ((!IsOverride || IsOffline) && !string.Equals(Release.Repository, Repository, StringComparison.OrdinalIgnoreCase))
            throw new SetupException($"The release says it is from {Release.Repository}, not {Repository}.");
        return Release;
    }

    /// The URL of an asset of the release (the real source only).
    public string AssetUrl(string asset) => $"{Endpoints.Web}/{Repository}/releases/download/{Release!.Tag}/{Uri.EscapeDataString(asset)}";

    /// Where a file of the release is in the folder: beside release.json, or (the tools) in a tools\ folder there.
    public static string? FindInFolder(string folder, string name)
    {
        foreach (var candidate in new[] { Path.Combine(folder, name), Path.Combine(folder, "tools", name) })
            if (File.Exists(candidate)) return candidate;
        return null;
    }

    /// What the folder has to hold besides release.json for a setup with no download: the SDK bundle and the four tool archives,
    /// each with its address and whether the folder has a file of that name and size (the checksum is checked when it is used).
    public IReadOnlyList<OfflineFile> OfflineFiles()
    {
        var release = Release ?? throw new InvalidOperationException("release.json is not loaded");
        var files = new List<(string Name, string Url, long Size)>();
        if (release.SdkBundle is { } bundle) files.Add((bundle.Asset, AssetUrl(bundle.Asset), bundle.Size));
        foreach (var name in ReleaseInfo.RequiredTools)
            if (release.Tools.TryGetValue(name, out var tool))
                files.Add((Path.GetFileName(new Uri(tool.Url).AbsolutePath), tool.Url, tool.Size));
        return files.Select(f =>
        {
            var found = LocalFolder == null ? null : FindInFolder(LocalFolder, f.Name);
            var present = false;
            try { present = found != null && new FileInfo(found).Length == f.Size; } catch (IOException) { }
            return new OfflineFile(f.Name, f.Url, f.Size, present);
        }).ToList();
    }

    /// Gets an asset into a file: copied from the override folder, or downloaded. The caller checks the SHA-256.
    public async Task FetchAssetAsync(string asset, string destination, IProgress<long> progress, CancellationToken ct) =>
        await DownloadAsync(LocalFolder != null ? Path.Combine(LocalFolder, asset) : AssetUrl(asset), destination, progress, ct,
                            allowLocalFile: LocalFolder != null, allowPlainHttp: Endpoints.AllowPlainHttp);

    /// Gets a file by https, or (allowLocalFile: only the development override's folder) copies it. Anything else
    /// that isn't https is refused: a path or a network share named by a release must never be opened.
    /// A failure of the network is a SetupException whose InnerException is the NetworkFailure (it names the stage and the
    /// host) and whose Detail is the report; Verified.FetchAsync tries such a download again.
    public static async Task DownloadAsync(string source, string destination, IProgress<long> progress, CancellationToken ct, bool allowLocalFile = false, bool allowPlainHttp = false)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        var buffer = new byte[1 << 20];
        long total = 0;
        if (!source.StartsWith("https://", StringComparison.OrdinalIgnoreCase) && !(allowPlainHttp && source.StartsWith("http://127.0.0.1", StringComparison.OrdinalIgnoreCase)))
        {
            if (!allowLocalFile) throw new SetupException("The release names a download that isn't https; it was not used.", source);
            if (!File.Exists(source)) throw new SetupException($"{Path.GetFileName(source)} is missing from the release source.");
            await using var input = new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.SequentialScan);
            await using var output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, FileOptions.SequentialScan);
            int read;
            while ((read = await input.ReadAsync(buffer, ct)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, read), ct);
                total += read;
                progress.Report(total);
            }
            return;
        }
        // No answer for 30 s, or no byte for 60 s: given up (and tried again by the caller, which has three tries).
        var name = Path.GetFileName(new Uri(source).AbsolutePath);
        var operation = new NetOperation($"download {name}");
        using var stall = CancellationTokenSource.CreateLinkedTokenSource(ct);
        try
        {
            using var response = await NetClient.GetAsync(source, operation, new NetRequest { AnswerTimeout = RequestTimeout, AllowPlainHttp = allowPlainHttp }, ct);
            if (!response.IsSuccessStatusCode) throw NetClient.StatusFailure(response, operation);
            Stream input;
            try { input = await response.Content.ReadAsStreamAsync(stall.Token); }
            catch (Exception ex) when (ex is IOException or HttpRequestException or OperationCanceledException && !ct.IsCancellationRequested)
            {
                throw NetClient.ReadFailure(ex, operation, stall.IsCancellationRequested, StallTimeout);
            }
            await using (input)
            {
                await using var output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, FileOptions.SequentialScan);
                int read;
                while (true)
                {
                    stall.CancelAfter(StallTimeout);
                    try { read = await input.ReadAsync(buffer, stall.Token); }
                    catch (Exception ex) when (ex is IOException or HttpRequestException or OperationCanceledException && !ct.IsCancellationRequested)
                    {
                        throw NetClient.ReadFailure(ex, operation, stall.IsCancellationRequested && !ct.IsCancellationRequested, StallTimeout);
                    }
                    if (read <= 0) break;
                    await output.WriteAsync(buffer.AsMemory(0, read), ct);
                    total += read;
                    progress.Report(total);
                }
            }
            operation.Log($"downloaded {total:N0} bytes");
        }
        catch (NetworkFailure failure)
        {
            throw new SetupException($"Couldn't download {name}. {failure.Plain}", operation.Report(failure), failure);
        }
    }
}

// release.json: what a release of the fork holds (scripts/ci/assemble-release.py writes it), and where the
// wizard gets it. The real source is the fork's latest published release on GitHub; a development override
// (a folder, or a file:// URL, with release.json and the assets) stands in for it, announced in the window.
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MclaLauncher.Setup;

sealed record ReleaseAsset(string Asset, string Sha256, long Size);

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
        foreach (var name in RequiredTools)
            if (!Tools.ContainsKey(name))
                throw new SetupException($"This release doesn't say where to get {name}.");
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

/// Where release.json and the assets come from.
sealed class ReleaseSource
{
    /// The fork's repository, as the real source and the clone name it.
    public const string Repository = "RenanQueiroz/mcla-recomp";
    public const string EnvironmentVariable = "MCLA_RELEASE_SOURCE";

    static readonly HttpClient Http = CreateClient();

    static HttpClient CreateClient()
    {
        var client = new HttpClient(new SocketsHttpHandler { AllowAutoRedirect = true, MaxAutomaticRedirections = 5, ConnectTimeout = TimeSpan.FromSeconds(20) })
        {
            Timeout = Timeout.InfiniteTimeSpan,
        };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("MclaLauncher/1.0");
        return client;
    }

    /// A folder, when the development override is in use; null for the real source.
    public string? LocalFolder { get; }
    public ReleaseInfo? Release { get; private set; }
    /// The git URL of the fork: GitHub's, or the override's (dev-source.json).
    public string GitUrl { get; private set; } = $"https://github.com/{Repository}.git";
    public bool IsOverride => LocalFolder != null;
    public string Describe => IsOverride
        ? $"a local release source: {LocalFolder}"
        : $"GitHub: {Repository}";

    /// The override from the command line or the environment: a folder or a file:// URL. Anything else is refused,
    /// so that the override can't quietly point the wizard at a web server.
    public static ReleaseSource FromOverride(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return new ReleaseSource(null);
        string folder = value.Trim();
        if (Uri.TryCreate(folder, UriKind.Absolute, out var uri) && uri.IsFile) folder = uri.LocalPath;
        else if (Uri.TryCreate(folder, UriKind.Absolute, out var other) && other.Scheme is "http" or "https")
            throw new SetupException("The development release source has to be a local folder or a file:// URL.");
        folder = Path.GetFullPath(folder);
        if (!Directory.Exists(folder)) throw new SetupException($"The development release source {folder} isn't a folder.");
        return new ReleaseSource(folder);
    }

    ReleaseSource(string? localFolder) => LocalFolder = localFolder;

    public async Task<ReleaseInfo> LoadAsync(CancellationToken ct)
    {
        string json;
        if (LocalFolder != null)
        {
            var file = Path.Combine(LocalFolder, "release.json");
            if (!File.Exists(file)) throw new SetupException($"There is no release.json in {LocalFolder}.");
            json = await File.ReadAllTextAsync(file, ct);
            var dev = Path.Combine(LocalFolder, "dev-source.json");
            if (File.Exists(dev))
            {
                using var doc = JsonDocument.Parse(await File.ReadAllTextAsync(dev, ct));
                if (doc.RootElement.TryGetProperty("gitUrl", out var url) && url.GetString() is { Length: > 0 } git) GitUrl = git;
            }
        }
        else
        {
            var url = $"https://github.com/{Repository}/releases/latest/download/release.json";
            try
            {
                using var response = await Http.GetAsync(url, ct);
                if (response.StatusCode == System.Net.HttpStatusCode.NotFound)
                    throw new SetupException("There is no release of the game's installer on GitHub yet. Try again later.", $"{url} answered 404");
                response.EnsureSuccessStatusCode();
                json = await response.Content.ReadAsStringAsync(ct);
            }
            catch (HttpRequestException ex)
            {
                throw new SetupException("Couldn't reach GitHub to find the latest release. Check the internet connection and try again.", ex.Message, ex);
            }
        }
        Release = ReleaseInfo.Parse(json);
        if (!IsOverride && !string.Equals(Release.Repository, Repository, StringComparison.OrdinalIgnoreCase))
            throw new SetupException($"The release says it is from {Release.Repository}, not {Repository}.");
        return Release;
    }

    /// The URL of an asset of the release (the real source only).
    public string AssetUrl(string asset) => $"https://github.com/{Repository}/releases/download/{Release!.Tag}/{Uri.EscapeDataString(asset)}";

    /// Gets an asset into a file: copied from the override folder, or downloaded. The caller checks the SHA-256.
    public async Task FetchAssetAsync(string asset, string destination, IProgress<long> progress, CancellationToken ct) =>
        await DownloadAsync(LocalFolder != null ? Path.Combine(LocalFolder, asset) : AssetUrl(asset), destination, progress, ct);

    public static async Task DownloadAsync(string source, string destination, IProgress<long> progress, CancellationToken ct)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        var buffer = new byte[1 << 20];
        long total = 0;
        if (!source.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
        {
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
        try
        {
            using var response = await Http.GetAsync(source, HttpCompletionOption.ResponseHeadersRead, ct);
            response.EnsureSuccessStatusCode();
            await using var input = await response.Content.ReadAsStreamAsync(ct);
            await using var output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, FileOptions.SequentialScan);
            int read;
            while ((read = await input.ReadAsync(buffer, ct)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, read), ct);
                total += read;
                progress.Report(total);
            }
        }
        catch (HttpRequestException ex)
        {
            throw new SetupException($"Couldn't download {Path.GetFileName(new Uri(source).AbsolutePath)}. Check the internet connection and try again.", $"{source}: {ex.Message}", ex);
        }
    }
}

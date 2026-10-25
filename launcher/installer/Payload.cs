// The launcher folder inside MCLA-Setup.exe.
//
// MCLA-Setup.exe is the installer program (this project, NativeAOT, about 3 MB) followed by the launcher's
// files as one Brotli stream and a 16-byte footer. Appending (rather than embedding as a resource) keeps the
// installer built once whatever the payload is, lets the tests put a tiny fake payload behind the real
// program, and costs nothing at run time: the program opens its own file and reads the end.
//
//   [installer program ...........................................]
//   [payload: "MCLAPK1\0", int32 meta length, meta (UTF-8 "key=value" lines), Brotli stream of the files]
//   [footer: "MCLAPAY1", int64 payload length]
//
// The files in the Brotli stream: int32 count, then for each file int32 path length, the path (UTF-8, with
// "/" separators), int64 size, the SHA-256 of its content (32 bytes), the content. Every file is hashed again
// as it is written, so a damaged download or a truncated file (a scanner stopped it) is found at once.
// Brotli at quality 9 with a 16 MB window: the launcher folder (183 MB) becomes 55 MB in about 20 s, 20% less
// than a zip of it, and unpacks in a second or two.
using System.Buffers;
using System.Security.Cryptography;
using System.Text;
using System.IO.Compression;

namespace Mcla.Setup;

sealed class PayloadMeta
{
    public Dictionary<string, string> Values { get; } = new(StringComparer.OrdinalIgnoreCase);
    public string Get(string key, string fallback = "") => Values.TryGetValue(key, out var v) ? v : fallback;
    public string Version => Get("version", "0.0.0");
    public string Tag => Get("tag", "");
    public string Commit => Get("commit", "");
    /// The launcher's program inside the folder.
    public string Exe => Get("exe", "MclaLauncher.exe");
    public long Bytes => long.TryParse(Get("bytes"), out var b) ? b : 0;
    public int Files => int.TryParse(Get("files"), out var f) ? f : 0;

    public byte[] ToBytes()
    {
        var sb = new StringBuilder();
        foreach (var (k, v) in Values) sb.Append(k).Append('=').Append(v.Replace('\n', ' ')).Append('\n');
        return Encoding.UTF8.GetBytes(sb.ToString());
    }

    public static PayloadMeta Parse(ReadOnlySpan<byte> bytes)
    {
        var meta = new PayloadMeta();
        foreach (var line in Encoding.UTF8.GetString(bytes).Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            var at = line.IndexOf('=');
            if (at > 0) meta.Values[line[..at].Trim()] = line[(at + 1)..].Trim();
        }
        return meta;
    }
}

static class PayloadFormat
{
    public static readonly byte[] Magic = "MCLAPK1\0"u8.ToArray();
    public static readonly byte[] FooterMagic = "MCLAPAY1"u8.ToArray();
    public const int FooterSize = 16;
    public const int MaxFiles = 20000;
    public const long MaxBytes = 2L * 1024 * 1024 * 1024;
}

/// A read-only window onto part of a stream (the payload inside the exe).
sealed class WindowStream : Stream
{
    readonly Stream inner;
    readonly long start, length;
    long position;
    public WindowStream(Stream inner, long start, long length) { this.inner = inner; this.start = start; this.length = length; }
    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => length;
    public override long Position { get => position; set => throw new NotSupportedException(); }
    public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));
    public override int Read(Span<byte> buffer)
    {
        var left = length - position;
        if (left <= 0) return 0;
        if (buffer.Length > left) buffer = buffer[..(int)left];
        if (inner.Position != start + position) inner.Position = start + position;
        var n = inner.Read(buffer);
        position += n;
        return n;
    }
    public override void Flush() { }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}

/// The payload of an exe (normally this program's own file).
sealed class Payload : IDisposable
{
    readonly FileStream file;
    readonly long start, length;
    public PayloadMeta Meta { get; }
    long streamStart;

    Payload(FileStream file, long start, long length, PayloadMeta meta, long streamStart)
    {
        this.file = file; this.start = start; this.length = length; Meta = meta; this.streamStart = streamStart;
    }

    /// Opens the payload at the end of `exePath`; null if the file has none (a bare installer program).
    public static Payload? Open(string exePath)
    {
        var file = new FileStream(exePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1 << 16, FileOptions.SequentialScan);
        try
        {
            if (file.Length < PayloadFormat.FooterSize + 16) { file.Dispose(); return null; }
            var footer = new byte[PayloadFormat.FooterSize];
            file.Position = file.Length - PayloadFormat.FooterSize;
            file.ReadExactly(footer);
            if (!footer.AsSpan(0, 8).SequenceEqual(PayloadFormat.FooterMagic)) { file.Dispose(); return null; }
            var length = BitConverter.ToInt64(footer, 8);
            var start = file.Length - PayloadFormat.FooterSize - length;
            if (length < 16 || start < 0) throw new InvalidDataException("the installer's payload is damaged (its length is wrong)");
            file.Position = start;
            var head = new byte[12];
            file.ReadExactly(head);
            if (!head.AsSpan(0, 8).SequenceEqual(PayloadFormat.Magic)) throw new InvalidDataException("the installer's payload is damaged (wrong header)");
            var metaLength = BitConverter.ToInt32(head, 8);
            if (metaLength < 0 || metaLength > 1 << 16) throw new InvalidDataException("the installer's payload is damaged (header)");
            var meta = new byte[metaLength];
            file.ReadExactly(meta);
            return new Payload(file, start, length, PayloadMeta.Parse(meta), start + 12 + metaLength);
        }
        catch { file.Dispose(); throw; }
    }

    /// Unpacks every file into `folder` (which must be new or empty), checking each one's SHA-256 and the
    /// total. `progress` gets (bytes done, bytes total); `cancelled` is looked at between files.
    public void Extract(string folder, Action<long, long>? progress = null, Func<bool>? cancelled = null)
    {
        Directory.CreateDirectory(folder);
        var root = Path.GetFullPath(folder).TrimEnd('\\') + "\\";
        var end = start + length;
        using var window = new WindowStream(file, streamStart, end - streamStart);
        using var brotli = new BrotliStream(window, CompressionMode.Decompress);
        var buffer = ArrayPool<byte>.Shared.Rent(1 << 20);
        try
        {
            var count = ReadInt32(brotli);
            if (count < 0 || count > PayloadFormat.MaxFiles) throw new InvalidDataException("the installer's payload is damaged (file count)");
            long total = Meta.Bytes, done = 0;
            var hash = new byte[32];
            for (var i = 0; i < count; i++)
            {
                if (cancelled?.Invoke() == true) throw new OperationCanceledException();
                var nameLength = ReadInt32(brotli);
                if (nameLength <= 0 || nameLength > 1024) throw new InvalidDataException("the installer's payload is damaged (a name)");
                var nameBytes = new byte[nameLength];
                brotli.ReadExactly(nameBytes);
                var relative = Encoding.UTF8.GetString(nameBytes);
                var size = ReadInt64(brotli);
                if (size < 0 || size > PayloadFormat.MaxBytes) throw new InvalidDataException("the installer's payload is damaged (a size)");
                brotli.ReadExactly(hash);
                var destination = Safe(root, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                using (var output = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 16))
                {
                    var left = size;
                    while (left > 0)
                    {
                        var n = brotli.Read(buffer, 0, (int)Math.Min(left, buffer.Length));
                        if (n <= 0) throw new InvalidDataException($"the installer's payload ends early (in {relative})");
                        sha.AppendData(buffer, 0, n);
                        output.Write(buffer, 0, n);
                        left -= n;
                        done += n;
                        progress?.Invoke(done, total);
                    }
                }
                if (!sha.GetHashAndReset().AsSpan().SequenceEqual(hash))
                    throw new InvalidDataException($"{relative} does not match its checksum: the installer file is damaged. Download it again.");
            }
        }
        finally { ArrayPool<byte>.Shared.Return(buffer); }
    }

    /// A relative path from the payload, resolved below `root`; anything that would leave it is refused.
    static string Safe(string root, string relative)
    {
        if (relative.Length == 0 || relative.StartsWith('/') || relative.Contains(':') || relative.Contains('\\') || relative.Contains('\0'))
            throw new InvalidDataException($"the installer's payload names a bad path: {relative}");
        foreach (var part in relative.Split('/'))
            if (part.Length == 0 || part == "." || part == ".." || part.EndsWith('.') || part.EndsWith(' '))
                throw new InvalidDataException($"the installer's payload names a bad path: {relative}");
        var full = Path.GetFullPath(Path.Combine(root, relative.Replace('/', '\\')));
        if (!full.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"the installer's payload names a bad path: {relative}");
        return full;
    }

    static int ReadInt32(Stream s) { Span<byte> b = stackalloc byte[4]; s.ReadExactly(b); return BitConverter.ToInt32(b); }
    static long ReadInt64(Stream s) { Span<byte> b = stackalloc byte[8]; s.ReadExactly(b); return BitConverter.ToInt64(b); }

    public void Dispose() => file.Dispose();
}

/// Builds MCLA-Setup.exe: the installer program, then the payload of a folder (build time, `--compose`).
static class PayloadWriter
{
    public static void Compose(string stubExe, string folder, string outExe, PayloadMeta meta, int quality, Action<string>? log = null)
    {
        folder = Path.GetFullPath(folder);
        var files = Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories)
            .Select(p => (Path: p, Relative: Path.GetRelativePath(folder, p).Replace('\\', '/')))
            .OrderBy(f => f.Relative, StringComparer.Ordinal).ToList();
        if (files.Count == 0 || files.Count > PayloadFormat.MaxFiles) throw new InvalidOperationException($"{files.Count} files in {folder}");
        // Every key is written, with its default when it was not given: a reader that is not this program (the
        // release's scripts read the header from the file) must not depend on this program's defaults.
        foreach (var key in new[] { "version", "tag", "commit", "exe" }) meta.Values.TryAdd(key, key switch { "version" => meta.Version, "tag" => meta.Tag, "commit" => meta.Commit, _ => meta.Exe });
        meta.Values["files"] = files.Count.ToString();
        meta.Values["bytes"] = files.Sum(f => new FileInfo(f.Path).Length).ToString();
        if (!files.Any(f => string.Equals(f.Relative, meta.Exe, StringComparison.OrdinalIgnoreCase)))
            throw new InvalidOperationException($"{meta.Exe} is not in {folder}");

        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outExe))!);
        using var output = new FileStream(outExe, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16);
        using (var stub = File.OpenRead(stubExe)) stub.CopyTo(output);
        var payloadStart = output.Position;
        output.Write(PayloadFormat.Magic);
        var metaBytes = meta.ToBytes();
        output.Write(BitConverter.GetBytes(metaBytes.Length));
        output.Write(metaBytes);

        var encoder = new BrotliEncoder(quality, 24);
        try
        {
            var outBuffer = new byte[1 << 20];
            void Feed(ReadOnlySpan<byte> data, bool final)
            {
                while (true)
                {
                    var status = encoder.Compress(data, outBuffer, out var consumed, out var written, final);
                    output.Write(outBuffer, 0, written);
                    data = data[consumed..];
                    if (status == OperationStatus.Done) { if (data.IsEmpty) return; }
                    else if (status == OperationStatus.NeedMoreData) { if (!final) return; }
                    else if (status == OperationStatus.InvalidData) throw new InvalidOperationException("Brotli refused the data");
                    // DestinationTooSmall: loop and write what it made
                }
            }
            Feed(BitConverter.GetBytes(files.Count), false);
            var chunk = new byte[1 << 20];
            var n = 0;
            foreach (var (path, relative) in files)
            {
                var info = new FileInfo(path);
                var name = Encoding.UTF8.GetBytes(relative);
                var header = new byte[4 + name.Length + 8];
                BitConverter.TryWriteBytes(header.AsSpan(0, 4), name.Length);
                name.CopyTo(header, 4);
                BitConverter.TryWriteBytes(header.AsSpan(4 + name.Length, 8), info.Length);
                byte[] hash;
                using (var input = File.OpenRead(path)) hash = SHA256.HashData(input);
                Feed(header, false);
                Feed(hash, false);
                using (var input = File.OpenRead(path))
                {
                    int read;
                    while ((read = input.Read(chunk, 0, chunk.Length)) > 0) Feed(chunk.AsSpan(0, read), false);
                }
                if (++n % 100 == 0) log?.Invoke($"{n}/{files.Count} files");
            }
            Feed(ReadOnlySpan<byte>.Empty, true);
        }
        finally { encoder.Dispose(); }

        var payloadLength = output.Position - payloadStart;
        output.Write(PayloadFormat.FooterMagic);
        output.Write(BitConverter.GetBytes(payloadLength));
    }
}

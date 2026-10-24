// Reading an Xbox 360 disc image (XDVDFS, "XGD2" and the other layouts a dump comes in): the
// logic of scripts/extract_game.py in C#, so that the setup wizard needs no Python. Read only;
// nothing is decrypted (the game's files are taken as they are on the disc).
using System.Buffers.Binary;

namespace MclaLauncher.Setup;

/// A file on the disc: its path from the root ("/default.xex"), where its data starts and how long it is.
sealed record DiscEntry(string Path, long Sector, long Size);

/// The file is not an Xbox 360 disc image (no XDVDFS volume at any of the known places), or it is cut
/// short or damaged inside its directories.
sealed class NotAnXboxDiscException(string message) : Exception(message);

sealed class XboxDiscImage : IDisposable
{
    public const int SectorSize = 0x800;
    static readonly byte[] Magic = "MICROSOFT*XBOX*MEDIA"u8.ToArray();
    // Where the game partition starts: a raw XDVDFS image, XGD2, XGD1, XGD3 and the redump layouts.
    static readonly long[] PartitionOffsets = [0x0, 0xFD90000, 0x2080000, 0x18300000, 0x89D80000];
    // The console's own updates, which the game does not use.
    static readonly string[] SkippedFolders = ["/$SystemUpdate"];
    // A real image has a few dozen files; this only stops a damaged one from running away.
    const int MaxEntries = 20000;

    readonly FileStream stream;
    readonly long partitionStart;

    public string Path { get; }
    public IReadOnlyList<DiscEntry> Entries { get; }

    XboxDiscImage(string path, FileStream stream, long partitionStart, IReadOnlyList<DiscEntry> entries)
    {
        Path = path;
        this.stream = stream;
        this.partitionStart = partitionStart;
        Entries = entries;
    }

    public static XboxDiscImage Open(string path)
    {
        var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16, FileOptions.RandomAccess);
        try
        {
            long length = stream.Length;
            foreach (var offset in PartitionOffsets)
            {
                if (offset + 32L * SectorSize + 28 > length) continue;
                var header = new byte[28];
                stream.Seek(offset + 32L * SectorSize, SeekOrigin.Begin);
                stream.ReadExactly(header);
                if (!header.AsSpan(0, 20).SequenceEqual(Magic)) continue;
                uint rootSector = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(20));
                uint rootSize = BinaryPrimitives.ReadUInt32LittleEndian(header.AsSpan(24));
                var entries = new List<DiscEntry>();
                Walk(stream, offset, rootSector, rootSize, "", entries, length, 0);
                entries.Sort((a, b) => string.CompareOrdinal(a.Path, b.Path));
                entries.RemoveAll(e => SkippedFolders.Any(s => e.Path.StartsWith(s, StringComparison.Ordinal)));
                return new XboxDiscImage(path, stream, offset, entries);
            }
            throw new NotAnXboxDiscException("There is no Xbox 360 disc file system in this file.");
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    // A directory is a binary tree of entries inside its sectors, each entry's links counted in
    // 4-byte units from the start of the directory; 0xFFFF in the left link marks an empty directory.
    static void Walk(FileStream stream, long partitionStart, uint sector, uint directorySize, string path,
                     List<DiscEntry> entries, long imageLength, int depth)
    {
        if (depth > 16) throw new NotAnXboxDiscException("The disc's folders are nested too deeply: the image is damaged.");
        long start = partitionStart + (long)sector * SectorSize;
        if (directorySize == 0 || directorySize > (1 << 24) || start + directorySize > imageLength)
            throw new NotAnXboxDiscException("A folder on the disc lies outside the image: the image is cut short or damaged.");
        var data = new byte[directorySize];
        stream.Seek(start, SeekOrigin.Begin);
        stream.ReadExactly(data);

        var visited = new HashSet<int>();
        var stack = new Stack<int>();
        stack.Push(0);
        while (stack.Count > 0)
        {
            int unit = stack.Pop();
            long offset = (long)unit * 4;
            if (offset + 14 > data.Length || !visited.Add(unit)) continue;
            var span = data.AsSpan((int)offset);
            ushort left = BinaryPrimitives.ReadUInt16LittleEndian(span);
            ushort right = BinaryPrimitives.ReadUInt16LittleEndian(span[2..]);
            uint entrySector = BinaryPrimitives.ReadUInt32LittleEndian(span[4..]);
            uint size = BinaryPrimitives.ReadUInt32LittleEndian(span[8..]);
            byte attributes = span[12];
            byte nameLength = span[13];
            if (left == 0xFFFF) continue;
            if (14 + nameLength > span.Length) continue;
            var name = System.Text.Encoding.Latin1.GetString(span.Slice(14, nameLength));
            var full = $"{path}/{name}";
            if ((attributes & 0x10) != 0)
            {
                if (size != 0) Walk(stream, partitionStart, entrySector, size, full, entries, imageLength, depth + 1);
            }
            else
            {
                if (entries.Count >= MaxEntries) throw new NotAnXboxDiscException("The disc lists more files than any game has: the image is damaged.");
                entries.Add(new DiscEntry(full, entrySector, size));
            }
            if (left != 0) stack.Push(left);
            if (right != 0) stack.Push(right);
        }
    }

    /// Opens a file's data as a stream of its own (several can be read in turn; not at once).
    public Stream OpenFile(DiscEntry entry)
    {
        long start = partitionStart + entry.Sector * SectorSize;
        if (start + entry.Size > stream.Length)
            throw new NotAnXboxDiscException($"{entry.Path.TrimStart('/')} lies beyond the end of the image: the image is cut short.");
        return new DiscFileStream(stream, start, entry.Size);
    }

    public void Dispose() => stream.Dispose();

    sealed class DiscFileStream(FileStream image, long start, long length) : Stream
    {
        long position;
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => length;
        public override long Position { get => position; set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));
        public override int Read(Span<byte> buffer)
        {
            long remaining = length - position;
            if (remaining <= 0) return 0;
            if (buffer.Length > remaining) buffer = buffer[..(int)remaining];
            image.Seek(start + position, SeekOrigin.Begin);
            int read = image.Read(buffer);
            position += read;
            return read;
        }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}

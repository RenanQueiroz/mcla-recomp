// The folder a path really is: what Windows opens when it follows every junction, symbolic link and substituted drive (subst) in the
// path, at any level and through chains of them, with 8.3 names written out in full and the \\?\ form removed. Used to compare the
// install folder with the launcher's folder: two spellings of one folder must compare equal.
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace MclaLauncher.Setup;

static class FinalPath
{
    const uint ShareAll = 7;                  // read | write | delete: opening a folder to ask its name must not disturb anyone
    const uint OpenExisting = 3;
    const uint BackupSemantics = 0x02000000;  // needed to open a directory

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle, StringBuilder buffer, uint size, uint flags);

    /// The final path of a folder that exists; null when it can't be opened. A drive root keeps its backslash (C:\).
    public static string? OfExisting(string path)
    {
        try
        {
            using var handle = CreateFileW(path, 0, ShareAll, IntPtr.Zero, OpenExisting, BackupSemantics, IntPtr.Zero);
            if (handle.IsInvalid) return null;
            var buffer = new StringBuilder(512);
            var length = GetFinalPathNameByHandleW(handle, buffer, (uint)buffer.Capacity, 0);
            if (length > buffer.Capacity)
            {
                buffer = new StringBuilder((int)length + 1);
                length = GetFinalPathNameByHandleW(handle, buffer, (uint)buffer.Capacity, 0);
            }
            if (length == 0 || length > buffer.Capacity) return null;
            var text = buffer.ToString();
            if (text.StartsWith(@"\\?\UNC\", StringComparison.Ordinal)) return @"\\" + text[8..];
            if (text.StartsWith(@"\\?\", StringComparison.Ordinal)) return text[4..];
            return text;
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or ArgumentException) { return null; }
    }

    /// The final path of a path that may not exist yet: the deepest folder of it that does exist is resolved, and the rest is
    /// appended as written. The path itself when nothing of it can be opened.
    public static string Resolve(string path)
    {
        string full;
        try { full = Path.GetFullPath(path); }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException) { return path; }
        var tail = new Stack<string>();
        var existing = full;
        string? real = null;
        while (existing is { Length: > 0 })
        {
            real = OfExisting(existing);
            if (real != null) break;
            var name = Path.GetFileName(existing.TrimEnd('\\'));
            if (name.Length == 0) return full;
            tail.Push(name);
            existing = Path.GetDirectoryName(existing.TrimEnd('\\')) ?? "";
        }
        if (real == null) return full;
        foreach (var name in tail) real = Path.Combine(real, name);
        return real;
    }
}

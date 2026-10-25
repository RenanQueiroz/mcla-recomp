// A Windows job object that kills what is in it when the launcher's process ends, however it ends (closed,
// killed from Task Manager, crashed): the setup's builds and tools must not go on writing into an install
// after the wizard that started them is gone. Used only by the setup wizard (Enabled), so a Classic or
// Installed launcher starts its builds exactly as before.
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace MclaLauncher.Setup;

static class ChildJob
{
    /// Set by the setup wizard; nothing is assigned to the job otherwise.
    public static volatile bool Enabled;

    static readonly object Gate = new();
    static IntPtr job;
    static bool failed;

    /// Puts a process (and so what it starts later) in the job, when the wizard has asked for it.
    public static void AssignIfEnabled(Process process)
    {
        if (!Enabled) return;
        try
        {
            var handle = Job();
            if (handle != IntPtr.Zero) AssignProcessToJobObject(handle, process.Handle);
        }
        catch (Exception ex) when (ex is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            // The process has already ended: nothing to guard.
        }
    }

    static IntPtr Job()
    {
        lock (Gate)
        {
            if (job != IntPtr.Zero || failed) return job;
            var handle = CreateJobObject(IntPtr.Zero, null);
            if (handle == IntPtr.Zero) { failed = true; return IntPtr.Zero; }
            var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION
            {
                BasicLimitInformation = new JOBOBJECT_BASIC_LIMIT_INFORMATION { LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE },
            };
            var size = Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>();
            var memory = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(info, memory, false);
                if (!SetInformationJobObject(handle, JobObjectExtendedLimitInformation, memory, (uint)size))
                {
                    CloseHandle(handle);
                    failed = true;
                    return IntPtr.Zero;
                }
            }
            finally { Marshal.FreeHGlobal(memory); }
            job = handle;
            return job;
        }
    }

    const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000;
    const int JobObjectExtendedLimitInformation = 9;

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct IO_COUNTERS
    {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateJobObject(IntPtr attributes, string? name);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool CloseHandle(IntPtr handle);
}

/// One setup at a time in an install folder: an exclusive lock file held open for the life of the setup (the
/// system lets go of it if the process dies). A second wizard on the same folder, or a wizard started while the
/// first still runs, is told so in words.
sealed class InstallLock : IDisposable
{
    public const string FileName = "install.lock";
    readonly FileStream stream;

    InstallLock(FileStream stream) => this.stream = stream;

    public static InstallLock Acquire(string root)
    {
        Directory.CreateDirectory(root);
        var file = Path.Combine(root, FileName);
        try
        {
            var stream = new FileStream(file, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 1, FileOptions.DeleteOnClose);
            try { stream.SetLength(0); var text = System.Text.Encoding.ASCII.GetBytes($"{Environment.ProcessId}\n"); stream.Write(text); stream.Flush(); }
            catch (IOException) { }
            return new InstallLock(stream);
        }
        catch (IOException ex)
        {
            throw new SetupException("A setup is already running in that folder (in another window of this program). Wait for it to finish, or close it, and try again.", $"{file}: {ex.Message}", ex);
        }
    }

    /// Another process holds the lock of that folder now.
    public static bool IsHeld(string root)
    {
        var file = Path.Combine(root, FileName);
        if (!File.Exists(file)) return false;
        try
        {
            using var probe = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            return false;
        }
        catch (IOException) { return true; }
        catch (UnauthorizedAccessException) { return false; }
    }

    public void Dispose() => stream.Dispose();
}

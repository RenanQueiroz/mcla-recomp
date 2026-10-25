// Shortcuts (.lnk files) through the shell's IShellLinkW / IPersistFile, called through their v-tables so that
// NativeAOT needs no COM interop code generation.
using System.Runtime.InteropServices;

namespace Mcla.Setup;

static unsafe class Shortcuts
{
    static readonly Guid CLSID_ShellLink = new("00021401-0000-0000-C000-000000000046");
    static readonly Guid IID_IShellLinkW = new("000214F9-0000-0000-C000-000000000046");
    static readonly Guid IID_IPersistFile = new("0000010B-0000-0000-C000-000000000046");

    [DllImport("ole32.dll")]
    static extern int CoInitializeEx(nint reserved, uint flags);
    [DllImport("ole32.dll")]
    static extern void CoUninitialize();
    [DllImport("ole32.dll")]
    static extern int CoCreateInstance(Guid* clsid, nint outer, uint context, Guid* iid, nint* result);

    // v-table slots
    const int QueryInterface = 0, Release = 2;
    const int GetPath = 3, SetDescription = 7, SetWorkingDirectory = 9, SetArguments = 11, SetIconLocation = 17, SetPath = 20;
    const int PersistLoad = 5, PersistSave = 6;

    /// The program a shortcut starts, or null if the file isn't a readable shortcut.
    public static string? ReadTarget(string lnk)
    {
        if (!File.Exists(lnk)) return null;
        return WithLink(link =>
        {
            var vtable = *(nint**)link;
            nint persist;
            var iid = IID_IPersistFile;
            if (((delegate* unmanaged[Stdcall]<nint, Guid*, nint*, int>)vtable[QueryInterface])(link, &iid, &persist) < 0) return null;
            try
            {
                fixed (char* path = lnk)
                    if (((delegate* unmanaged[Stdcall]<nint, char*, uint, int>)(*(nint**)persist)[PersistLoad])(persist, path, 0) < 0) return null;
                var buffer = stackalloc char[1024];
                // SLGP_RAWPATH (4): the path as stored, without environment variables expanded
                if (((delegate* unmanaged[Stdcall]<nint, char*, int, nint, uint, int>)vtable[GetPath])(link, buffer, 1024, 0, 4) < 0) return null;
                return new string(buffer);
            }
            finally { ((delegate* unmanaged[Stdcall]<nint, uint>)(*(nint**)persist)[Release])(persist); }
        });
    }

    /// Writes (or replaces) a shortcut. Returns false if the shell refused.
    public static bool Create(string lnk, string target, string workingDirectory, string icon, string description)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(lnk)!);
        return WithLink(link =>
        {
            var vtable = *(nint**)link;
            int hr;
            fixed (char* t = target) hr = ((delegate* unmanaged[Stdcall]<nint, char*, int>)vtable[SetPath])(link, t);
            if (hr < 0) return false;
            fixed (char* w = workingDirectory) ((delegate* unmanaged[Stdcall]<nint, char*, int>)vtable[SetWorkingDirectory])(link, w);
            fixed (char* d = description) ((delegate* unmanaged[Stdcall]<nint, char*, int>)vtable[SetDescription])(link, d);
            fixed (char* i = icon) ((delegate* unmanaged[Stdcall]<nint, char*, int, int>)vtable[SetIconLocation])(link, i, 0);
            nint persist;
            var iid = IID_IPersistFile;
            if (((delegate* unmanaged[Stdcall]<nint, Guid*, nint*, int>)vtable[QueryInterface])(link, &iid, &persist) < 0) return false;
            try
            {
                fixed (char* path = lnk)
                    hr = ((delegate* unmanaged[Stdcall]<nint, char*, int, int>)(*(nint**)persist)[PersistSave])(persist, path, 1);
                return hr >= 0;
            }
            finally { ((delegate* unmanaged[Stdcall]<nint, uint>)(*(nint**)persist)[Release])(persist); }
        });
    }

    static T WithLink<T>(Func<nint, T> action)
    {
        var hr = CoInitializeEx(0, 2 /* COINIT_APARTMENTTHREADED */);
        var initialized = hr >= 0; // S_FALSE (1) is "already initialised on this thread": still balanced by CoUninitialize
        try
        {
            nint link;
            var clsid = CLSID_ShellLink;
            var iid = IID_IShellLinkW;
            if (CoCreateInstance(&clsid, 0, 1 /* CLSCTX_INPROC_SERVER */, &iid, &link) < 0 || link == 0) return default!;
            try { return action(link); }
            finally { ((delegate* unmanaged[Stdcall]<nint, uint>)(*(nint**)link)[Release])(link); }
        }
        finally { if (initialized) CoUninitialize(); }
    }
}

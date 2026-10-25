// The installer's little windows: message boxes and a progress dialog (a Task Dialog with a progress bar).
// Plain Win32 through P/Invoke, so that NativeAOT needs nothing else. The progress dialog is a convenience:
// if it can't be shown, the installer runs without it.
using System.Runtime.InteropServices;

namespace Mcla.Setup;

static unsafe class Dialogs
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int MessageBoxW(nint owner, string text, string caption, uint type);
    [DllImport("user32.dll")]
    static extern nint SendMessageW(nint window, uint message, nuint wParam, nint lParam);
    [DllImport("user32.dll")]
    static extern bool PostMessageW(nint window, uint message, nuint wParam, nint lParam);
    [DllImport("comctl32.dll")]
    static extern int TaskDialogIndirect(TaskDialogConfig* config, int* button, int* radio, int* verify);

    public const string Title = "Midnight Club: Los Angeles";

    const uint MB_OK = 0, MB_OKCANCEL = 1, MB_YESNO = 4, MB_ICONERROR = 0x10, MB_ICONQUESTION = 0x20, MB_ICONINFORMATION = 0x40, MB_SETFOREGROUND = 0x10000;
    const int IDOK = 1, IDCANCEL = 2, IDYES = 6;

    public static void Error(string text) => MessageBoxW(0, text, Title, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    public static void Info(string text) => MessageBoxW(0, text, Title, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    public static bool YesNo(string text) => MessageBoxW(0, text, Title, MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
    public static bool OkCancel(string text) => MessageBoxW(0, text, Title, MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDOK;

    // ---- the progress dialog ----------------------------------------------------------------------------------

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    struct TaskDialogConfig
    {
        public uint Size;
        public nint Parent;
        public nint Instance;
        public int Flags;
        public int CommonButtons;
        public char* WindowTitle;
        public nint MainIcon;
        public char* MainInstruction;
        public char* Content;
        public uint ButtonCount;
        public nint Buttons;
        public int DefaultButton;
        public uint RadioButtonCount;
        public nint RadioButtons;
        public int DefaultRadioButton;
        public char* VerificationText;
        public char* ExpandedInformation;
        public char* ExpandedControlText;
        public char* CollapsedControlText;
        public nint FooterIcon;
        public char* Footer;
        public nint Callback;
        public nint CallbackData;
        public uint Width;
    }

    const int TDF_ALLOW_DIALOG_CANCELLATION = 0x8, TDF_SHOW_PROGRESS_BAR = 0x200, TDF_CALLBACK_TIMER = 0x800, TDF_CAN_BE_MINIMIZED = 0x8000;
    const int TDCBF_CANCEL_BUTTON = 8;
    const uint TDM_CLICK_BUTTON = 1126, TDM_SET_PROGRESS_BAR_RANGE = 1129, TDM_SET_PROGRESS_BAR_POS = 1130, TDM_SET_ELEMENT_TEXT = 1132, TDM_ENABLE_BUTTON = 1135;
    const uint TDN_CREATED = 0, TDN_BUTTON_CLICKED = 2;
    const int TDE_CONTENT = 0, TDE_MAIN_INSTRUCTION = 3;

    static nint window;
    static volatile bool cancelRequested, closing;
    static readonly ManualResetEventSlim created = new(false);

    /// True once the person has pressed Cancel.
    public static bool CancelRequested => cancelRequested;

    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvStdcall)])]
    static nint Callback(nint hwnd, uint notification, nuint wParam, nint lParam, nint data)
    {
        if (notification == TDN_CREATED)
        {
            window = hwnd;
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, (nint)(1000 << 16));
            created.Set();
            if (closing) PostMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0); // the work finished before the dialog was up
        }
        else if (notification == TDN_BUTTON_CLICKED && (int)wParam == IDCANCEL)
        {
            if (closing) return 0; // the installer is done: let the dialog go
            cancelRequested = true;
            return 1; // S_FALSE: the dialog stays; the installer closes it once it has stopped
        }
        return 0;
    }

    /// Shows the dialog on this thread and returns when `Close` is called. Run the work on another thread.
    public static void ShowProgress(string instruction, string content)
    {
        fixed (char* title = Title)
        fixed (char* main = instruction)
        fixed (char* text = content)
        {
            var config = new TaskDialogConfig
            {
                Size = (uint)sizeof(TaskDialogConfig),
                Flags = TDF_SHOW_PROGRESS_BAR | TDF_ALLOW_DIALOG_CANCELLATION | TDF_CAN_BE_MINIMIZED,
                CommonButtons = TDCBF_CANCEL_BUTTON,
                WindowTitle = title,
                MainInstruction = main,
                Content = text,
                Callback = (nint)(delegate* unmanaged[Stdcall]<nint, uint, nuint, nint, nint, nint>)&Callback,
            };
            int button, radio, verify;
            try { TaskDialogIndirect(&config, &button, &radio, &verify); }
            catch (Exception) { created.Set(); /* no dialog; the work goes on without one */ }
        }
    }

    /// Waits (a few seconds at most) until the dialog exists, so that the work can update it.
    public static bool WaitForDialog(int milliseconds) => created.Wait(milliseconds) && window != 0;

    /// 0 to 1000.
    public static void SetProgress(int permille)
    {
        if (window != 0) SendMessageW(window, TDM_SET_PROGRESS_BAR_POS, (nuint)Math.Clamp(permille, 0, 1000), 0);
    }

    public static unsafe void SetText(string content)
    {
        if (window == 0) return;
        fixed (char* p = content) SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, (nint)p);
    }

    public static void SetInstruction(string text)
    {
        if (window == 0) return;
        fixed (char* p = text) SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION, (nint)p);
    }

    /// Cancel can no longer stop it (the swap has begun).
    public static void DisableCancel()
    {
        if (window != 0) SendMessageW(window, TDM_ENABLE_BUTTON, IDCANCEL, 0);
    }

    /// Closes the dialog (from any thread).
    public static void Close()
    {
        closing = true;
        if (window != 0) PostMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0);
    }
}

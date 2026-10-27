// Text onto the clipboard, for the "Copy details" buttons (the network details of a failed fetch).
using System.Runtime.InteropServices;
using Windows.ApplicationModel.DataTransfer;

namespace MclaLauncher;

static class ClipboardText
{
    /// Puts the text on the clipboard. Returns null, or why it could not.
    public static string? Copy(string text)
    {
        try
        {
            var package = new DataPackage();
            package.SetText(text);
            Clipboard.SetContent(package);
            return null;
        }
        catch (Exception ex) when (ex is COMException or InvalidOperationException or UnauthorizedAccessException) { return ex.Message; }
    }
}

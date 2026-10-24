// Development only (MCLA_DEV_SHOTS=<folder>): a PNG of the window's content, so that the wizard's pages can be
// looked at without anyone at the screen. (The page draws a solid background while this is on: the system's
// backdrop is not part of what a bitmap of the content holds.) Does nothing unless the variable is set.
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.WindowsRuntime;
using Microsoft.UI.Reactor;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;
using MclaLauncher.Setup;

namespace MclaLauncher;

static class WizardShots
{
    static int count;

    public static async Task TakeAsync(string name)
    {
        var folder = DevOptions.Current.ShotsDir;
        if (folder == null) return;
        try
        {
            // After the page has been laid out.
            await Task.Delay(900);
            var dispatcher = ReactorApp.UIDispatcher;
            var done = new TaskCompletionSource();
            dispatcher.TryEnqueue(async () =>
            {
                try
                {
                    if (ReactorApp.PrimaryWindow?.NativeWindow.Content is not UIElement content) return;
                    var bitmap = new RenderTargetBitmap();
                    await bitmap.RenderAsync(content);
                    var pixels = await bitmap.GetPixelsAsync();
                    using var stream = new InMemoryRandomAccessStream();
                    var encoder = await BitmapEncoder.CreateAsync(BitmapEncoder.PngEncoderId, stream);
                    encoder.SetPixelData(BitmapPixelFormat.Bgra8, BitmapAlphaMode.Premultiplied, (uint)bitmap.PixelWidth, (uint)bitmap.PixelHeight, 96, 96, WindowsRuntimeBufferExtensions.ToArray(pixels));
                    await encoder.FlushAsync();
                    stream.Seek(0);
                    var bytes = new byte[stream.Size];
                    await stream.ReadAsync(bytes.AsBuffer(), (uint)bytes.Length, InputStreamOptions.None);
                    Directory.CreateDirectory(folder);
                    await File.WriteAllBytesAsync(Path.Combine(folder, $"{Interlocked.Increment(ref count):000}-{name}.png"), bytes);
                }
                catch (Exception ex) when (ex is IOException or InvalidOperationException or COMException or UnauthorizedAccessException)
                {
                    System.Diagnostics.Debug.WriteLine($"shot: {ex.Message}");
                }
                finally { done.TrySetResult(); }
            });
            await done.Task;
        }
        catch (Exception ex) when (ex is InvalidOperationException or TaskCanceledException) { }
    }
}

// A virus scanner or the search indexer may hold a file or folder the setup has just written (a 2 GB archive,
// a folder of twenty thousand files), and then a rename or a delete fails for a moment with a sharing violation
// or "access denied". These wait a little and try again, for about ten seconds in all, and only for those errors.
namespace MclaLauncher.Setup;

static class Retry
{
    public static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    /// Not worth waiting for: the file or folder isn't there, or the disk is full.
    static bool Worth(Exception ex) => ex switch
    {
        FileNotFoundException or DirectoryNotFoundException or PathTooLongException => false,
        IOException io when io.HResult is unchecked((int)0x80070070) or unchecked((int)0x80070027) => false,
        IOException or UnauthorizedAccessException => true,
        _ => false,
    };

    /// Cancelling ends the wait at once (an OperationCanceledException), not after the ten seconds.
    public static void Io(Action action, TimeSpan? patience = null, CancellationToken ct = default)
    {
        var until = DateTime.UtcNow + (patience ?? Patience);
        for (var wait = 50; ; wait = Math.Min(wait * 2, 500))
        {
            try { action(); return; }
            catch (Exception ex) when (Worth(ex) && DateTime.UtcNow < until)
            {
                if (ct.WaitHandle.WaitOne(wait)) ct.ThrowIfCancellationRequested();
            }
        }
    }

    public static T Io<T>(Func<T> action, TimeSpan? patience = null, CancellationToken ct = default)
    {
        T result = default!;
        Io(() => result = action(), patience, ct);
        return result;
    }

    public static async Task IoAsync(Func<Task> action, CancellationToken ct, TimeSpan? patience = null)
    {
        var until = DateTime.UtcNow + (patience ?? Patience);
        for (var wait = 50; ; wait = Math.Min(wait * 2, 500))
        {
            try { await action(); return; }
            catch (Exception ex) when (Worth(ex) && DateTime.UtcNow < until) { await Task.Delay(wait, ct); }
        }
    }
}

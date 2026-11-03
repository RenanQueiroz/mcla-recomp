// When Windows refuses to start the game: Smart App Control, or an App Control for Business (WDAC) or
// Software Restriction policy on a managed PC, only lets programs with a signature (or a known good
// one) run. The game is built on the player's PC and is not signed, so the start fails with a Win32
// error that the .NET exception renders as "An Application Control policy has blocked this file."
// This file turns that into words the player can act on. Kept free of the launcher's UI, for the
// setup wizard and the checks.
using System.ComponentModel;

namespace MclaLauncher;

static class AppControl
{
    /// ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION: Smart App Control, or an App Control policy.
    public const int SystemIntegrityPolicyViolation = 4551;
    /// ERROR_ACCESS_DISABLED_BY_POLICY: a group policy or Software Restriction Policy.
    public const int AccessDisabledByPolicy = 1260;

    /// Whether Windows refused to start a program because of such a policy (the exception, or what it wraps).
    public static bool IsBlock(Exception? ex)
    {
        for (var e = ex; e != null; e = e.InnerException)
        {
            if (e is Win32Exception { NativeErrorCode: SystemIntegrityPolicyViolation or AccessDisabledByPolicy }) return true;
        }
        return false;
    }

    /// What to tell the player; `then` names what to press after turning it off ("Try again", "Play").
    public static string Message(string then) =>
        "Windows blocked the game: Smart App Control (or the PC's administrator policy) blocks programs without a signature, "
        + "and the setup builds the game on this PC, so it has none. "
        + $"To allow it: Windows Security > App & browser control > Smart App Control settings > Off, then {then}. "
        + "On some versions of Windows, Smart App Control can't be turned back on later without reinstalling Windows. "
        + "On a school or work PC, only its administrator can allow it.";
}

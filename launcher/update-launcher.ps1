# Rebuilds the launcher after an update changed it, then starts it again.
# The launcher starts this and quits; -WaitFor is its process id, so its
# files are free to be replaced by the time build-launcher.ps1 runs.
param([int]$WaitFor = 0)

$ErrorActionPreference = "Stop"
try {
    if ($WaitFor) {
        "waiting for the launcher to close"
        Wait-Process -Id $WaitFor -ErrorAction SilentlyContinue
    }
    & "$PSScriptRoot\build-launcher.ps1" -NoShortcut
    Start-Process "$PSScriptRoot\app\MclaLauncher.exe"
} catch {
    Write-Host ""
    Write-Host "FAILED: $($_.Exception.Message)" -ForegroundColor Red
    Read-Host "Press Enter to close"
    exit 1
}

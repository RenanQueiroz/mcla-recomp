# Builds the launcher (src\, a Reactor / WinUI 3 app) into app\ and puts a
# shortcut to it on the desktop. Only needed after changing the launcher.
#
#   .\build-launcher.ps1               build, publish and (re)create the desktop shortcut
#   .\build-launcher.ps1 -NoShortcut   build and publish only
#
# Needs the .NET 10 SDK: winget install Microsoft.DotNet.SDK.10
param([switch]$NoShortcut)

$ErrorActionPreference = "Stop"
$app = Join-Path $PSScriptRoot "app"
$exe = Join-Path $app "MclaLauncher.exe"

# The first dotnet that has a 10.x SDK.
$candidates = @((Get-Command dotnet -ErrorAction SilentlyContinue).Source,
                "$env:ProgramFiles\dotnet\dotnet.exe") | Where-Object { $_ -and (Test-Path $_) }
$dotnet = $candidates | Where-Object { (& $_ --list-sdks) -match '^10\.' } | Select-Object -First 1
if (-not $dotnet) { throw "no .NET 10 SDK found; install it with: winget install Microsoft.DotNet.SDK.10" }
"dotnet: $dotnet"

if (Get-Process MclaLauncher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
    throw "the launcher is running; close it first so its files can be replaced"
}

& $dotnet publish "$PSScriptRoot\src\MclaLauncher.csproj" -c Release -r win-x64 -p:Platform=x64 -o $app
if ($LASTEXITCODE -ne 0) { throw "build failed" }

if (-not $NoShortcut) {
    $desktop = [Environment]::GetFolderPath("Desktop")
    $shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut("$desktop\Midnight Club Los Angeles.lnk")
    $shortcut.TargetPath = $exe
    $shortcut.WorkingDirectory = $app
    # The .ico rather than the exe: Explorer caches icons per path, so after an
    # icon change a shortcut pointing at the same exe keeps showing the old one.
    $shortcut.IconLocation = "$app\launcher.ico,0"
    $shortcut.Description = "Update, build and play the Midnight Club: Los Angeles recompilation"
    $shortcut.Save()
    # Tell Explorer icons changed (SHCNE_ASSOCCHANGED) so the desktop redraws it.
    Add-Type -Namespace Shell -Name Notify -MemberDefinition '[DllImport("shell32.dll")] public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);'
    [Shell.Notify]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
    "shortcut: $desktop\Midnight Club Los Angeles.lnk"
}
"launcher: $exe"

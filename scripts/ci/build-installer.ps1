# Builds MCLA-Setup.exe: the installer program (launcher\installer, NativeAOT, about 2.5 MB) with the published
# launcher folder appended to it as its payload (see launcher\installer\Payload.cs).
#
#   powershell -File scripts\ci\build-installer.ps1 -LauncherDir <published folder> -Out <folder>
#              [-Version 1.2.3] [-Tag v1.2.3] [-Commit <40 hex>] [-Quality 9] [-StubOut <file>]
#
# -LauncherDir is what scripts\ci\publish-launcher.ps1 made (the launcher, self-contained, with its notices).
# Writes <Out>\MCLA-Setup.exe, then checks what it made: the program is one native NativeAOT file with no .NET
# runtime in it, its header says the version, tag and commit given, and unpacking it gives back every file of the
# launcher folder, byte for byte. -StubOut keeps a copy of the bare installer program (no payload), which the
# tests in launcher\dev\update-check build their own payloads behind.
param(
    [Parameter(Mandatory)][string]$LauncherDir,
    [Parameter(Mandatory)][string]$Out,
    [string]$Version = "0.0.0",
    [string]$Tag = "",
    [string]$Commit = "",
    [int]$Quality = 9,
    [string]$StubOut = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$project = "$repo\launcher\installer\MclaSetup.csproj"
if (-not (Test-Path -LiteralPath "$LauncherDir\MclaLauncher.exe")) { throw "$LauncherDir is not a published launcher (no MclaLauncher.exe)" }
$LauncherDir = (Resolve-Path -LiteralPath $LauncherDir).ProviderPath

$dotnet = (Get-Command dotnet -ErrorAction Stop).Source
if ((& $dotnet --version) -notmatch '^10\.') { throw "the .NET 10 SDK is needed" }

# NativeAOT links with the C++ tools of Visual Studio, which it finds through vswhere.
if (-not (Get-Command vswhere.exe -ErrorAction SilentlyContinue)) {
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
    if (Test-Path -LiteralPath "$installer\vswhere.exe") { $env:PATH = "$env:PATH;$installer" }
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("mcla-installer-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
try {
    $stubDir = "$work\stub"
    & $dotnet publish $project -c Release -r win-x64 -p:Version=$Version -p:ContinuousIntegrationBuild=true --nologo -v:minimal -o $stubDir
    if ($LASTEXITCODE -ne 0) { throw "dotnet publish of the installer program failed ($LASTEXITCODE)" }
    $stub = "$stubDir\MCLA-Setup.exe"
    $extra = @(Get-ChildItem -LiteralPath $stubDir -File | Where-Object { $_.Name -ne "MCLA-Setup.exe" -and $_.Extension -notin ".pdb" })
    if ($extra) { throw "the installer program is not one native file: $($extra.Name -join ', ')" }
    "installer program: {0:N2} MB" -f ((Get-Item $stub).Length / 1MB)
    if ($StubOut) {
        New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($StubOut))) | Out-Null
        Copy-Item -LiteralPath $stub -Destination $StubOut -Force
    }

    New-Item -ItemType Directory -Force $Out | Out-Null
    $exe = Join-Path (Resolve-Path -LiteralPath $Out).ProviderPath "MCLA-Setup.exe"
    $compose = @("--compose", $LauncherDir, $exe, "--stub", $stub, "--quality", "$Quality", "--version", $Version)
    if ($Tag) { $compose += @("--tag", $Tag) }
    if ($Commit) { $compose += @("--commit", $Commit) }
    # A console program from a terminal: the exe is a GUI-subsystem program, so its output is read through the pipe.
    $output = & $stub @compose | Out-String
    if ($LASTEXITCODE -ne 0) { throw "compose failed:`n$output" }
    $output.TrimEnd()

    # What was made.
    $info = (& $exe --info | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "--info failed:`n$info" }
    foreach ($want in @("version=$Version", $(if ($Tag) { "tag=$Tag" }), $(if ($Commit) { "commit=$Commit" }))) {
        if ($want -and $info -notmatch "(?m)^$([regex]::Escape($want))\s*$") { throw "the payload header lacks $want`n$info" }
    }
    $unpacked = "$work\unpacked"
    $check = (& $exe --extract $unpacked | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "--extract failed:`n$check" }
    $files = Get-ChildItem -LiteralPath $LauncherDir -Recurse -File
    $count = 0
    foreach ($f in $files) {
        $relative = $f.FullName.Substring($LauncherDir.Length).TrimStart('\')
        $copy = Join-Path $unpacked $relative
        if (-not (Test-Path -LiteralPath $copy)) { throw "unpacking lost $relative" }
        if ((Get-Item -LiteralPath $copy).Length -ne $f.Length -or
            (Get-FileHash -LiteralPath $copy -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash) { throw "unpacking changed $relative" }
        $count++
    }
    if ((Get-ChildItem -LiteralPath $unpacked -Recurse -File).Count -ne $count) { throw "unpacking made files the launcher folder does not have" }
    # The header as the release's assemble step reads it, from the file (scripts\ci\check-installer.py, the same
    # reader): every key has to be in the file itself, not filled in by this program's own defaults.
    $read = @("-I", "$PSScriptRoot\check-installer.py", $exe, "--version", $Version, "--files", "$count")
    if ($Tag) { $read += @("--tag", $Tag) }
    if ($Commit) { $read += @("--commit", $Commit) }
    $readOutput = (& python @read | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "check-installer.py failed:`n$readOutput" }
    $readOutput.TrimEnd()
    # The program imports only what Windows has.
    $bytes = [IO.File]::ReadAllBytes($stub)
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    foreach ($bad in "coreclr.dll", "hostfxr.dll", "clrjit.dll") {
        if ($text.Contains($bad)) { throw "the installer program refers to ${bad}: it is not NativeAOT" }
    }
    "MCLA-Setup.exe: {0:N1} MB, {1} launcher files verified by unpacking them again" -f ((Get-Item $exe).Length / 1MB), $count
} finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

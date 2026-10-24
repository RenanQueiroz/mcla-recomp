# Publishes the launcher the way a release ships it: self-contained (the .NET runtime and the
# Windows App SDK's runtime in its folder, so the player installs nothing), with its notices.
#
#   powershell -File scripts\ci\publish-launcher.ps1 -Out <folder> [-Zip <file.zip>] [-Version 1.2.3]
#
# Builds with the .NET 10 SDK on the machine (the dependencies float, as for a build on a
# player's PC: the versions this build resolved are in notices\packages.json and in release.json),
# writes source-commit.txt as build-launcher.ps1 does, puts the licence and notice files of the
# launcher and of every package in the folder's notices\, checks that what the launcher needs at
# start is in the folder, and zips it (deterministically) when -Zip is given.
param([Parameter(Mandatory)][string]$Out, [string]$Zip = "", [string]$Version = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$project = "$repo\launcher\src\MclaLauncher.csproj"

$dotnet = (Get-Command dotnet -ErrorAction Stop).Source
$sdkVersion = (& $dotnet --version)
if ($sdkVersion -notmatch '^10\.') { throw "the .NET 10 SDK is needed; this is $sdkVersion" }
"dotnet SDK $sdkVersion"

# -Out is emptied: it must be a folder of its own, not the repository or a folder that holds it.
if (Test-Path -LiteralPath $Out) {
    $outFull = ((Resolve-Path -LiteralPath $Out).ProviderPath.TrimEnd('\') + '\')
    $repoFull = ($repo.TrimEnd('\') + '\')
    if ($repoFull.StartsWith($outFull, [StringComparison]::OrdinalIgnoreCase) -or $outFull.Length -le 4) {
        throw "-Out ($Out) is the repository, a folder that holds it, or a drive's root: it would be emptied"
    }
    Remove-Item -LiteralPath $Out -Recurse -Force
}
$extra = @()
if ($Version) { $extra += "-p:Version=$Version" }
& $dotnet publish $project -c Release -r win-x64 -p:Platform=x64 -p:SelfContained=true -p:ContinuousIntegrationBuild=true --nologo -v:minimal -o $Out @extra
if ($LASTEXITCODE -ne 0) { throw "dotnet publish failed ($LASTEXITCODE)" }
$Out = (Resolve-Path -LiteralPath $Out).ProviderPath

# For the launcher to know which source it was built from, as a build on the player's PC leaves it.
$source = & git.exe -C $repo log -1 --format=%H -- launcher/src launcher/build-launcher.ps1
if ($LASTEXITCODE -ne 0 -or -not $source) { throw "git log could not name the commit of the launcher's source (a shallow clone?)" }
Set-Content -LiteralPath "$Out\source-commit.txt" -Value $source -Encoding ASCII

python -I "$PSScriptRoot\launcher-notices.py" $Out "$Out\notices"
if ($LASTEXITCODE -ne 0) { throw "launcher-notices.py failed" }

# What the unpackaged WinUI app needs to start, and what self-contained adds.
$need = "MclaLauncher.exe", "MclaLauncher.dll", "MclaLauncher.pri", "MclaLauncher.deps.json", "MclaLauncher.runtimeconfig.json",
        "coreclr.dll", "hostfxr.dll", "System.Private.CoreLib.dll", "Microsoft.WindowsAppRuntime.Bootstrap.dll",
        "Microsoft.ui.xaml.dll", "Microsoft.WinUI.dll", "launcher.ico", "source-commit.txt"
$missing = @($need | Where-Object { -not (Test-Path -LiteralPath "$Out\$_") })
if ($missing) { throw "the published launcher lacks: $($missing -join ', ')" }
$runtimeConfig = Get-Content -LiteralPath "$Out\MclaLauncher.runtimeconfig.json" -Raw
if ($runtimeConfig -notmatch 'includedFrameworks') { throw "the launcher is not self-contained (runtimeconfig.json has no includedFrameworks)" }

$files = Get-ChildItem -LiteralPath $Out -Recurse -File
"launcher: $($files.Count) files, $([int](($files | Measure-Object Length -Sum).Sum / 1MB)) MB in $Out"

if ($Zip) {
    New-Item -ItemType Directory -Force (Split-Path $Zip -Parent) | Out-Null
    python -I "$PSScriptRoot\zipdir.py" $Out $Zip
    if ($LASTEXITCODE -ne 0) { throw "zipdir.py failed" }
}

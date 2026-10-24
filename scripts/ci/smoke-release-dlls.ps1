# The in-game check of a release's FidelityFX DLLs, before the release is published: the game, built
# on this PC, runs FSR anti-aliasing and FSR frame generation on Direct3D 12 and on Vulkan with the
# two DLLs of the release's SDK bundle in place of its own, through launcher\dev\devrun.py (a scratch
# folder for the game's data; the real saves are not touched), and each run has to log that the
# runtime was loaded and that FSR and frame generation are ready. The DLLs the build had are put back.
#
#   powershell -File scripts\ci\smoke-release-dlls.ps1 -Bundle <the SDK bundle zip of the draft> [-DevRoot <folder>]
#
# Needs the game built here (out\build\win-amd64-release\mcla.exe, scripts\make_pc.ps1) and the GPU
# to itself while it runs (about 2 minutes). The four runs: FSR anti-aliasing on d3d12 and on vulkan,
# and FSR anti-aliasing with frame generation on each.
param([Parameter(Mandatory)][string]$Bundle, [string]$DevRoot = "", [string]$GameData = "", [string]$Script = "launcher\dev\scripts\boot-to-roam.pad")

$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$build = "$repo\out\build\win-amd64-release"
if (-not (Test-Path -LiteralPath "$build\mcla.exe")) { throw "no game built here ($build\mcla.exe): run scripts\make_pc.ps1 first" }
if (-not (Test-Path -LiteralPath $Bundle)) { throw "no such file: $Bundle" }

# The DLLs of the bundle (a zip, or an unpacked folder).
$stage = Join-Path ([IO.Path]::GetTempPath()) "mcla-smoke-dlls"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
if ((Get-Item -LiteralPath $Bundle).PSIsContainer) {
    foreach ($n in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll") { Copy-Item -LiteralPath "$Bundle\bin\$n" -Destination $stage }
} else {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Bundle).ProviderPath)
    try {
        foreach ($n in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll") {
            $entry = $zip.GetEntry("bin/$n")
            if (-not $entry) { throw "$Bundle has no bin/$n" }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, "$stage\$n", $true)
        }
    } finally { $zip.Dispose() }
}
foreach ($n in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll") {
    "{0}  {1:N0} bytes  sha256 {2}" -f $n, (Get-Item "$stage\$n").Length, (Get-FileHash -Algorithm SHA256 "$stage\$n").Hash.ToLowerInvariant()
}

if (-not $GameData) { $GameData = "$repo\game" }
$env:MCLA_EXE = "$build\mcla.exe"
$env:MCLA_GAME_DATA = $GameData
if ($DevRoot) { $env:MCLA_DEV_ROOT = $DevRoot }
$runs = if ($DevRoot) { "$DevRoot\runs" } else { "$env:LOCALAPPDATA\mcla-dev\runs" }

$cases = @(
    @{ Name = "FSR, Direct3D 12";                    Backend = "d3d12";  Extra = @("--mcla_anti_aliasing=fsr");                              Expect = @("loaded amd_fidelityfx_dx12.dll", "FSR 3.1 ready for .* \(Direct3D 12\)") }
    @{ Name = "FSR, Vulkan";                         Backend = "vulkan"; Extra = @("--mcla_anti_aliasing=fsr");                              Expect = @("loaded amd_fidelityfx_vk.dll", "FSR 3.1 ready for .* \(Vulkan\)") }
    @{ Name = "FSR and frame generation, Direct3D 12"; Backend = "d3d12";  Extra = @("--mcla_anti_aliasing=fsr", "--frame_generation=fsr");  Expect = @("loaded amd_fidelityfx_dx12.dll", "(?i)frame generation: ready for") }
    @{ Name = "FSR and frame generation, Vulkan";      Backend = "vulkan"; Extra = @("--mcla_anti_aliasing=fsr", "--frame_generation=fsr");  Expect = @("loaded amd_fidelityfx_vk.dll", "(?i)frame generation: ready for") }
)
$bad = '(?i)couldn.t be loaded|RUNTIME NOT LOADED|FSR is unavailable|frame generation can.t run'

# The build's own DLLs are kept and put back.
$backup = "$stage\original"
New-Item -ItemType Directory -Force $backup | Out-Null
foreach ($n in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll") {
    if (Test-Path -LiteralPath "$build\$n") { Copy-Item -LiteralPath "$build\$n" -Destination $backup }
}
$failures = 0
try {
    foreach ($n in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll") { Copy-Item -LiteralPath "$stage\$n" -Destination $build -Force }
    foreach ($case in $cases) {
        "=== $($case.Name)"
        $before = if (Test-Path -LiteralPath $runs) { @(Get-ChildItem -LiteralPath $runs -Directory | ForEach-Object Name) } else { @() }
        Push-Location $repo
        try {
            $output = & python -I launcher\dev\devrun.py run --script $Script --backend $case.Backend --timeout 240 --gpu-wait 0 --extra @($case.Extra) | ForEach-Object { "$_" }
            $exit = $LASTEXITCODE
        } finally { Pop-Location }
        $output | Select-Object -Last 4
        $run = @(Get-ChildItem -LiteralPath $runs -Directory | Where-Object { $before -notcontains $_.Name } | Sort-Object Name) | Select-Object -Last 1
        $log = if ($run -and (Test-Path -LiteralPath "$($run.FullName)\game.log")) { Get-Content -LiteralPath "$($run.FullName)\game.log" } else { @() }
        $ok = ($exit -eq 0) -and $log.Count -gt 0
        foreach ($pattern in $case.Expect) {
            $hit = @($log | Where-Object { $_ -match $pattern })
            "  {0}: {1}" -f $(if ($hit) { "ok     " } else { "MISSING" }), $pattern
            if (-not $hit) { $ok = $false }
        }
        $problems = @($log | Where-Object { $_ -match $bad })
        foreach ($p in $problems) { "  PROBLEM: $p"; $ok = $false }
        if (-not $ok) { $failures++ }
        "  result: $(if ($ok) { 'PASS' } else { 'FAIL' }) (devrun exit $exit, $($run.FullName))"
    }
} finally {
    foreach ($n in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll") {
        if (Test-Path -LiteralPath "$backup\$n") { Copy-Item -LiteralPath "$backup\$n" -Destination $build -Force } else { Remove-Item -LiteralPath "$build\$n" -ErrorAction SilentlyContinue }
    }
}
"smoke-release-dlls: $($cases.Count - $failures) of $($cases.Count) runs passed"
if ($failures) { exit 1 }
$global:LASTEXITCODE = 0

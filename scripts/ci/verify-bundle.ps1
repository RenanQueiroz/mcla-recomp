# Looks at an unpacked SDK bundle (the folder make_pc.ps1 -SdkBundle unpacks into) the way a player's
# build will use it, without a game:
#   - rexglue.exe runs and says what it is (its --help exits with -1: the tool wants a command);
#   - the two FidelityFX DLLs import nothing of the Visual C++ or C runtime (read from the files,
#     with llvm-readobj), and the Direct3D 12 one loads (the Vulkan one needs a Vulkan loader,
#     vulkan-1.dll, which a runner without a GPU does not have: it is tried, and only said);
#   - a small program (scripts\ci\linktest) is built against the bundle's CMake package with llvm-mingw,
#     linked against rexruntime.dll's import library, and run with the bundle's DLLs (libc++ and
#     libunwind included): the headers, the import library, the DLLs and the toolchain agree.
#
#   powershell -File scripts\ci\verify-bundle.ps1 -SdkDir third_party\rexglue-sdk-bin [-Work <folder>]
param([Parameter(Mandatory)][string]$SdkDir, [string]$Work = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. "$repo\scripts\env.ps1" -Toolchain mingw
if (-not $env:MCLA_CC -or -not (Test-Path -LiteralPath $env:MCLA_CC)) { throw "no llvm-mingw (scripts\make_pc.ps1 -Toolchain mingw -InstallTools installs it)" }
$sdk = (Resolve-Path -LiteralPath $SdkDir).ProviderPath
$bin = "$sdk\bin"
if (-not $Work) { $Work = Join-Path $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }) "verify-bundle" }
if (Test-Path -LiteralPath $Work) { Remove-Item -LiteralPath $Work -Recurse -Force }
New-Item -ItemType Directory -Force $Work | Out-Null

# --- rexglue.exe
$help = (& "$bin\rexglue.exe" --help) -join "`n"
$help.Split("`n") | Select-Object -First 3
if ($help -notmatch 'ReXGlue v\d') { throw "rexglue.exe does not run, or does not say what it is" }

# --- the FidelityFX DLLs
$readobj = Join-Path (Split-Path $env:MCLA_CC -Parent) "llvm-readobj.exe"
$banned = '^(msvcp\d+(_\d+)?|vcruntime\d+(_\d+)?|concrt\d+|ucrtbase|api-ms-win-crt-.*)\.dll$'
Add-Type -Namespace Win -Name Loader -MemberDefinition '[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibraryW(string path);'
foreach ($dll in "amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll") {
    $text = @(& $readobj --coff-imports "$bin\$dll")
    if ($LASTEXITCODE -ne 0) { throw "llvm-readobj could not read $dll ($LASTEXITCODE)" }
    $imports = @($text | ForEach-Object { if ($_ -match '(?i)^\s*Name:\s+(\S+\.dll)\s*$') { $Matches[1].ToLowerInvariant() } } | Sort-Object -Unique)
    "$dll imports: $($imports -join ', ')"
    if ($imports -notcontains "kernel32.dll") { throw "$dll : the import list was not read" }
    $crt = @($imports | Where-Object { $_ -match $banned })
    if ($crt) { throw "$dll imports the C runtime ($($crt -join ', ')): it needs the Visual C++ Redistributable" }
    $handle = [Win.Loader]::LoadLibraryW("$bin\$dll")
    "$dll loads: $($handle -ne [IntPtr]::Zero)$(if ($handle -eq [IntPtr]::Zero) { ' (error ' + [Runtime.InteropServices.Marshal]::GetLastWin32Error() + ')' })"
    # The Vulkan one needs a Vulkan loader, which a machine without a GPU driver has none of.
    if ($handle -eq [IntPtr]::Zero -and $dll -like "*dx12*") { throw "$dll does not load" }
}

# --- the link test
$build = "$Work\build"
cmake -S "$repo\scripts\ci\linktest" -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$($sdk.Replace('\', '/'))" "-DCMAKE_CXX_COMPILER=$env:MCLA_CXX"
if ($LASTEXITCODE -ne 0) { throw "cmake could not configure the link test against $sdk" }
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "the link test does not build against $sdk" }
# The bundle's DLLs beside it, as mcla.exe has them.
Get-ChildItem -LiteralPath $bin -Filter "*.dll" | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $build -Force }
$result = (& "$build\link_test.exe") -join "`n"
$result
if ($LASTEXITCODE -ne 0 -or $result -notmatch 'link test ok') { throw "the link test program does not run ($LASTEXITCODE)" }
"verify-bundle: ok"
# The last native command's exit code is what a runner's PowerShell step ends with.
$global:LASTEXITCODE = 0

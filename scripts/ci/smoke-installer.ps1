# Uses MCLA-Setup.exe the way a player would, on scratch folders, without starting the launcher (a runner has no
# one to look at its window): install, install again over it, remove it. Shortcuts go to a scratch folder too.
#
#   powershell -File scripts\ci\smoke-installer.ps1 -Exe <MCLA-Setup.exe>
#
# The installer's own scenarios (an update that waits for the old launcher, the way back when the new one does not
# start, a damaged file, a held file, ...) are launcher\dev\update-check, which CI runs on the bare installer program.
param([Parameter(Mandatory)][string]$Exe)

$ErrorActionPreference = "Stop"
$Exe = (Resolve-Path -LiteralPath $Exe).ProviderPath
$root = Join-Path ([IO.Path]::GetTempPath()) ("mcla-smoke-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$app = Join-Path $root "Programs\MCLA"
$lnk = Join-Path $root "lnk"

# A GUI-subsystem program: Start-Process -Wait is what waits for it and gives its exit code.
function Run-Setup([string[]]$Arguments) {
    $p = Start-Process -FilePath $Exe -ArgumentList $Arguments -Wait -PassThru -WindowStyle Hidden
    $p.ExitCode
}
function Expect([bool]$Ok, [string]$What) {
    if (-not $Ok) {
        $log = Join-Path ([IO.Path]::GetTempPath()) "MCLA-Setup.log"
        if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log | ForEach-Object { "  setup log: $_" } }
        throw "installer smoke test: $What"
    }
    "ok: $What"
}

try {
    $common = @("--silent", "--dir", "`"$app`"", "--shortcut-dir", "`"$lnk`"", "--no-launch")
    $code = Run-Setup $common
    Expect ($code -eq 0) "a first install exits 0 (it was $code)"
    foreach ($name in "MclaLauncher.exe", "MclaLauncher.dll", "MclaLauncher.pri", "MclaLauncher.deps.json", "launcher.ico",
                      "source-commit.txt", "installed.txt", "coreclr.dll", "Microsoft.ui.xaml.dll", "notices\LAUNCHER-LICENSE.txt") {
        Expect (Test-Path -LiteralPath (Join-Path $app $name)) "the install holds $name"
    }
    Expect ((Get-ChildItem -LiteralPath $app -Recurse -File).Count -ge 300) "the install holds the launcher's files"
    Expect (@(Get-ChildItem -LiteralPath $lnk -Recurse -Filter *.lnk).Count -eq 2) "a Start menu and a desktop shortcut were made (in the scratch folder)"
    $shell = New-Object -ComObject WScript.Shell
    $target = $shell.CreateShortcut((Get-ChildItem -LiteralPath $lnk -Recurse -Filter *.lnk | Select-Object -First 1).FullName).TargetPath
    Expect ($target -eq (Join-Path $app "MclaLauncher.exe")) "the shortcut starts the installed launcher"
    # Shows what the installed.txt of the installer says.
    Get-Content -LiteralPath (Join-Path $app "installed.txt") | ForEach-Object { "  installed.txt: $_" }

    Set-Content -LiteralPath (Join-Path $app "install.json") -Value '{"installRoot":"C:\\MCLA"}'
    $code = Run-Setup $common
    Expect ($code -eq 0) "installing over it exits 0 (it was $code)"
    Expect ((Get-Content -LiteralPath (Join-Path $app "install.json") -Raw) -match "installRoot") "the pointer the wizard writes (install.json) survived"
    Expect (-not (Get-ChildItem -LiteralPath (Split-Path $app) -Directory | Where-Object { $_.Name -like "MCLA.*" })) "no .old, .new or .failed folder was left"

    $code = Run-Setup @("--uninstall", "--silent", "--dir", "`"$app`"", "--shortcut-dir", "`"$lnk`"")
    Expect ($code -eq 0) "uninstall exits 0 (it was $code)"
    Expect (-not (Test-Path -LiteralPath $app)) "the folder is gone"
    Expect (@(Get-ChildItem -LiteralPath $lnk -Recurse -Filter *.lnk).Count -eq 0) "its shortcuts are gone"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

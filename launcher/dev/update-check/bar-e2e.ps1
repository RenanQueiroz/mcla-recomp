# The real launcher updating itself through its update bar, end to end, on scratch folders and with no game: an older
# launcher (installed by MCLA-Setup.exe, or unpacked from a zip) in Installed mode, a local release source that names a
# newer launcher and holds its MCLA-Setup.exe, and UI Automation clicking the bar's button.
#
#   powershell -File bar-e2e.ps1 -Stub <the bare MCLA-Setup.exe> -Launcher <a published launcher folder>
#              -ReleaseJson <a release.json whose tag is in the git repository> -GitUrl <that repository> [-Way InPlace|Move|Other] [-Keep]
#              [-NewSetup <a real MCLA-Setup.exe> -NewSourceCommit <its launcher's source commit> -NewVersion <its version>]
#
# -Launcher is what scripts\ci\publish-launcher.ps1 made: the "old" and the "new" launcher are copies of it that differ in
# source-commit.txt (a bar appears when the release's launcher.sourceCommit is not the running one's), so what is tried is the
# mechanism, not a code change. With -NewSetup the "new" launcher is a real release's: its MCLA-Setup.exe (a draft's, say) is the
# installer the release source offers, release.json's launcher.sourceCommit is -NewSourceCommit (that release's own release.json says
# which), and what must be true afterwards is that this installer's launcher is the one that runs. -ReleaseJson/-GitUrl: a fixture of the wizard's tests (the checkout is made from the repository at the
# tag, so the install has nothing to update but the launcher). Nothing outside the scratch folder is written: the installer is told
# where to put things with MCLA_SETUP_DIR and MCLA_SETUP_SHORTCUT_DIR. The launchers it starts are closed through their windows.
#
#   InPlace  the old launcher was installed by Setup: the bar says "Update the launcher", and the new launcher takes its folder.
#   Move     the old launcher was unpacked from a zip: "Move to the new launcher", and the new one is installed in Setup's folder
#            with the old one's install.json as its pointer (the old folder stays).
param(
    [Parameter(Mandatory)][string]$Stub,
    [Parameter(Mandatory)][string]$Launcher,
    [Parameter(Mandatory)][string]$ReleaseJson,
    [Parameter(Mandatory)][string]$GitUrl,
    [ValidateSet('InPlace', 'Move', 'Other')][string]$Way = 'InPlace',
    [string]$NewSetup = '',
    [string]$NewSourceCommit = '',
    [string]$NewVersion = '0.2.0',
    [string]$Scratch = (Join-Path ([IO.Path]::GetTempPath()) 'mcla-bar-e2e'),
    [switch]$Keep
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$Stub = (Resolve-Path -LiteralPath $Stub).ProviderPath
$Launcher = (Resolve-Path -LiteralPath $Launcher).ProviderPath
$Scratch = [IO.Path]::GetFullPath($Scratch)
if ((Split-Path $Scratch -Leaf) -notlike 'mcla-bar-e2e*') { throw "-Scratch has to be a folder named mcla-bar-e2e*" }
$failures = 0
function Check([bool]$ok, [string]$what) { if (-not $ok) { $script:failures++ }; "  {0} {1}" -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $what }
function Run([string]$exe, [string[]]$arguments) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -WindowStyle Hidden
    $p.WaitForExit(300000) | Out-Null
    $p.ExitCode
}

# ---- UI Automation ------------------------------------------------------------------------------------------------------
function Find-Ui([int]$procId, [string]$like) {
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $procId)
    foreach ($e in [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)) {
        try { if ($e.Current.Name -like $like) { return $e } } catch { }
    }
    $null
}
function Wait-Ui([int]$procId, [string]$like, [int]$seconds) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) { $e = Find-Ui $procId $like; if ($e) { return $e }; Start-Sleep -Milliseconds 1500 }
    $null
}
function Launchers([string]$folder) {
    @(Get-Process MclaLauncher -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($folder + '\', [StringComparison]::OrdinalIgnoreCase) })
}

# ---- the scratch folder ---------------------------------------------------------------------------------------------------
if (Test-Path -LiteralPath $Scratch) { Remove-Item -LiteralPath $Scratch -Recurse -Force }
New-Item -ItemType Directory -Force $Scratch | Out-Null
$release = Get-Content -LiteralPath $ReleaseJson -Raw | ConvertFrom-Json
$oldCommit = 'a' * 40
$newCommit = if ($NewSetup) { $NewSourceCommit } else { 'b' * 40 }
if ($NewSetup -and $newCommit -notmatch '^[0-9a-f]{40}$') { throw '-NewSetup needs -NewSourceCommit (40 hex digits)' }

function Make-Launcher([string]$name, [string]$sourceCommit) {
    $dir = Join-Path $Scratch "payload-$name"
    Copy-Item -LiteralPath $Launcher -Destination $dir -Recurse
    Set-Content -LiteralPath (Join-Path $dir 'source-commit.txt') -Value $sourceCommit -Encoding ASCII
    $dir
}
function Make-Setup([string]$name, [string]$version) {
    $dir = Make-Launcher $name $(if ($name -eq 'old') { $oldCommit } else { $newCommit })
    $exe = Join-Path $Scratch "MCLA-Setup-$name.exe"
    $code = Run $Stub @('--compose', "`"$dir`"", "`"$exe`"", '--stub', "`"$Stub`"", '--quality', '1', '--version', $version, '--tag', "e2e-$name")
    if ($code -ne 0 -or -not (Test-Path -LiteralPath $exe)) { throw "compose of the $name installer failed ($code)" }
    $exe
}
$setupOld = Make-Setup 'old' '0.1.0'
$setupNew = if ($NewSetup) { (Resolve-Path -LiteralPath $NewSetup).ProviderPath } else { Make-Setup 'new' $NewVersion }

# The release source: release.json says the launcher is the new one and names the new installer.
$source = Join-Path $Scratch 'release'
New-Item -ItemType Directory -Force $source | Out-Null
Copy-Item -LiteralPath $setupNew -Destination (Join-Path $source 'MCLA-Setup.exe')
$hash = (Get-FileHash -LiteralPath $setupNew -Algorithm SHA256).Hash.ToLowerInvariant()
$release.launcher = [pscustomobject]@{ asset = 'MCLA-Launcher-e2e-win-x64.zip'; sha256 = ('0' * 63) + '1'; size = 1; sourceCommit = $newCommit }
$release | Add-Member -NotePropertyName installer -NotePropertyValue ([pscustomobject]@{ asset = 'MCLA-Setup.exe'; sha256 = $hash; size = (Get-Item -LiteralPath $setupNew).Length }) -Force
$release | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $source 'release.json') -Encoding UTF8
Set-Content -LiteralPath (Join-Path $source 'dev-source.json') -Value ('{"gitUrl": "' + ($GitUrl -replace '\\', '/') + '"}') -Encoding ASCII

# The install: a checkout at the release's tag (so there is no game update to offer), a record that says it is finished.
$root = Join-Path $Scratch 'install'
New-Item -ItemType Directory -Force $root | Out-Null
& git clone --quiet $GitUrl (Join-Path $root 'mcla-recomp') 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "git clone of $GitUrl failed" }
& git -C (Join-Path $root 'mcla-recomp') -c advice.detachedHead=false checkout --quiet $release.tag 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) { throw "the tag $($release.tag) is not in $GitUrl" }
$record = [ordered]@{ schema = 1; installRoot = $root; state = 'ready'; channel = 'release'; toolchain = 'mingw'; releaseTag = $release.tag; releaseCommit = $release.commit; gameMode = 'copy' }
$recordJson = $record | ConvertTo-Json
Set-Content -LiteralPath (Join-Path $root 'install.json') -Value $recordJson -Encoding UTF8

# The old launcher and where the new one will go.
$programs = Join-Path $Scratch 'Programs\MCLA'
$lnk = Join-Path $Scratch 'lnk'
if ($Way -eq 'InPlace') {
    $oldFolder = $programs
    $code = Run $setupOld @('--silent', '--no-launch', '--dir', "`"$programs`"", '--shortcut-dir', "`"$lnk`"")
    if ($code -ne 0) { throw "installing the old launcher failed ($code)" }
} else {
    $oldFolder = Join-Path $Scratch 'zip-launcher'
    $code = Run $setupOld @('--extract', "`"$oldFolder`"")
    if ($code -ne 0) { throw "unpacking the old launcher failed ($code)" }
}
Set-Content -LiteralPath (Join-Path $oldFolder 'install.json') -Value $recordJson -Encoding UTF8
$env:MCLA_RELEASE_SOURCE = $source
$env:MCLA_SETUP_DIR = $programs
$env:MCLA_SETUP_SHORTCUT_DIR = $lnk

"way: $Way; old launcher in $oldFolder; release $($release.tag), its launcher is $($newCommit.Substring(0, 8))"
if ($Way -eq 'Other') {
    # A zip launcher while Setup's folder already holds a launcher of its own: the bar offers its folder, never a move.
    $code = Run $setupOld @('--silent', '--no-launch', '--dir', "`"$programs`"", '--shortcut-dir', "`"$lnk`"")
    if ($code -ne 0) { throw "installing the launcher in Setup's folder failed ($code)" }
}
$oldProcess = Start-Process -FilePath (Join-Path $oldFolder 'MclaLauncher.exe') -PassThru
try {
    $bar = Wait-Ui $oldProcess.Id '*A newer launcher*' 90
    Check ($null -ne $bar) $(if ($bar) { "the bar is shown: $($bar.Current.Name)" } else { 'the update bar is shown' })
    $wantButton = switch ($Way) { 'InPlace' { 'Update the launcher' } 'Move' { 'Move to the new launcher' } default { 'Open its folder' } }
    $button = Wait-Ui $oldProcess.Id $wantButton 10
    Check ($null -ne $button) "the button says '$wantButton'"
    if (-not $button) { throw 'no button to click' }
    if ($Way -eq 'Other') {
        # Not clicked (it opens a folder window). Nothing may have been started or changed.
        Check ($bar.Current.Name -like '*already in*') 'the bar says a launcher installed by the setup is already there'
        Start-Sleep -Seconds 3
        Check ((-not $oldProcess.HasExited) -and (-not (Get-Process 'MCLA-Setup' -ErrorAction SilentlyContinue)) -and ((Get-Content -LiteralPath (Join-Path $programs 'source-commit.txt') -Raw).Trim() -eq $oldCommit)) 'no installer was started and the launcher in Setup''s folder is as it was'
        throw 'done'
    }
    $button.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    "clicked at $(Get-Date -Format T)"

    # The old launcher quits, the installer works, the new launcher starts.
    Check ($oldProcess.WaitForExit(60000)) 'the old launcher quit after the click'
    $newFolder = $programs
    $newExe = Join-Path $newFolder 'MclaLauncher.exe'
    $deadline = (Get-Date).AddSeconds(90)
    $new = @()
    while ((Get-Date) -lt $deadline -and $new.Count -eq 0) { Start-Sleep -Milliseconds 700; $new = Launchers $newFolder }
    Check ($new.Count -gt 0) 'the new launcher started'
    if ($new.Count -gt 0) {
        Start-Sleep -Seconds 9      # its ready file (5 s after it is up), the installer's commit, the old folder deleted
        $new = Launchers $newFolder
        Check ($new.Count -gt 0) 'the new launcher is still running after the installer has taken it as started'
        $installed = if (Test-Path (Join-Path $newFolder 'installed.txt')) { Get-Content (Join-Path $newFolder 'installed.txt') -Raw } else { '' }
        Check ($installed -match ('version=' + [regex]::Escape($NewVersion))) "installed.txt names the new installer ($NewVersion)"
        Check ((Get-Content -LiteralPath (Join-Path $newFolder 'source-commit.txt') -Raw).Trim() -eq $newCommit) 'the new launcher is the new one (source-commit.txt)'
        $pointer = Get-Content -LiteralPath (Join-Path $newFolder 'install.json') -Raw | ConvertFrom-Json
        Check ($pointer.installRoot -eq $root -and $pointer.state -eq 'ready') 'the new launcher has the pointer to the same install'
        Check (-not (Get-ChildItem (Split-Path $newFolder) -Directory | Where-Object { $_.Name -like 'MCLA.old-*' -or $_.Name -like 'MCLA.failed-*' })) 'no .old or .failed folder is left beside it'
        if ($Way -eq 'Move') {
            Check ((Test-Path (Join-Path $oldFolder 'MclaLauncher.exe')) -and ((Get-Content -LiteralPath (Join-Path $oldFolder 'source-commit.txt') -Raw).Trim() -eq $oldCommit)) 'the old folder is left as it was'
            Check (@(Get-ChildItem $lnk -Recurse -Filter *.lnk -ErrorAction SilentlyContinue).Count -eq 2) 'shortcuts were made (in the scratch folder)'
        }
        $again = Find-Ui $new[0].Id '*A newer launcher*'
        $deadline = (Get-Date).AddSeconds(25)
        while ($null -eq (Find-Ui $new[0].Id '*Midnight Club*') -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
        Start-Sleep -Seconds 10
        Check ($null -eq (Find-Ui $new[0].Id '*A newer launcher*')) 'the new launcher shows no update bar (it is the release''s launcher)'
        $downloads = $programs + '.download'
        $gone = $false
        $deadline = (Get-Date).AddSeconds(80)    # its clean-up runs a minute after a launcher the installer started
        while ((Get-Date) -lt $deadline) { if (-not (Test-Path -LiteralPath $downloads)) { $gone = $true; break }; Start-Sleep -Seconds 4 }
        Check $gone 'the downloaded installer was cleaned up by the new launcher'
        Check (-not (Test-Path -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) 'MCLA.download'))) 'nothing was downloaded into the temp folder'
    }
}
catch { if ($_.Exception.Message -ne 'done') { $failures++; "  FAIL $($_.Exception.Message)" } }
finally {
    foreach ($folder in @($oldFolder, $programs)) {
        foreach ($proc in Launchers $folder) { $null = $proc.CloseMainWindow(); $proc.WaitForExit(8000) | Out-Null }
    }
    Remove-Item Env:MCLA_RELEASE_SOURCE, Env:MCLA_SETUP_DIR, Env:MCLA_SETUP_SHORTCUT_DIR -ErrorAction SilentlyContinue
}
if (-not $Keep) { Start-Sleep -Seconds 2; Remove-Item -LiteralPath $Scratch -Recurse -Force -ErrorAction SilentlyContinue }
"{0} checks failed" -f $failures
exit $(if ($failures) { 1 } else { 0 })

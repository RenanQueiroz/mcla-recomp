# Parses every PowerShell script of the repository with the parser of the PowerShell it is run by
# (Windows PowerShell 5.1 on a runner: what a player's PC has), and fails on a syntax error.
#   powershell -File scripts\ci\check-powershell.ps1
$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$bad = 0
$count = 0
foreach ($f in Get-ChildItem -LiteralPath $repo -Recurse -File -Include *.ps1, *.psm1 |
        Where-Object { $_.FullName -notmatch '\\(third_party|out|tools|\.git)\\' }) {
    $count++
    $errors = $null; $tokens = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($f.FullName, [ref]$tokens, [ref]$errors)
    foreach ($e in $errors) { "$($f.FullName):$($e.Extent.StartLineNumber): $($e.Message)"; $bad++ }
}
"check-powershell: $count scripts, $bad syntax error(s), PowerShell $($PSVersionTable.PSVersion)"
if ($bad) { exit 1 }

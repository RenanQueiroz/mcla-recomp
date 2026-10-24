# Parses every PowerShell script of the repository, and the PowerShell in the workflows' `run:` steps
# (those with `shell: powershell`), with the parser of the PowerShell it is run by (Windows
# PowerShell 5.1 on a runner: what a player's PC has), and fails on a syntax error.
#   powershell -File scripts\ci\check-powershell.ps1
$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$bad = 0
$count = 0

function Check([string]$name, [string]$text) {
    $errors = $null; $tokens = $null
    [void][System.Management.Automation.Language.Parser]::ParseInput($text, [ref]$tokens, [ref]$errors)
    foreach ($e in $errors) { "${name}:$($e.Extent.StartLineNumber): $($e.Message)"; $script:bad++ }
    $script:count++
}

foreach ($f in Get-ChildItem -LiteralPath $repo -Recurse -File |
        Where-Object { $_.Extension -in ".ps1", ".psm1" -and $_.FullName -notmatch '\\(third_party|out|tools|\.git)\\' }) {
    Check $f.FullName ([IO.File]::ReadAllText($f.FullName))
}

# The steps of the workflows: a step runs from its "- " line to the next one at the same indent; the
# script is the block under `run: |` (or the rest of a one-line `run:`) of a step with `shell: powershell`.
foreach ($wf in Get-ChildItem -LiteralPath "$repo\.github\workflows" -Filter *.yml) {
    $lines = [IO.File]::ReadAllLines($wf.FullName)
    $steps = New-Object System.Collections.Generic.List[object]
    $current = $null
    # Steps start with "- " at one indent (the least of the lines that start a step with a key of its own).
    $indent = ($lines | ForEach-Object { if ($_ -match '^(\s*)- (name|uses|id|run|shell):') { $Matches[1].Length } } | Measure-Object -Minimum).Minimum
    foreach ($line in $lines) {
        if ($line -match '^(\s*)- ' -and $Matches[1].Length -eq $indent) {
            $current = New-Object System.Collections.Generic.List[string]
            $steps.Add($current)
        }
        if ($null -ne $current) { $current.Add($line) }
    }
    $number = 0
    foreach ($step in $steps) {
        $number++
        if (-not ($step | Where-Object { $_ -match '^\s*shell:\s*powershell\s*$' })) { continue }
        for ($i = 0; $i -lt $step.Count; $i++) {
            if ($step[$i] -match '^(\s*)(?:- )?run:\s*(\|[-+]?)?\s*(.*)$') {
                $runIndent = $Matches[1].Length
                if ($Matches[2]) {
                    $body = New-Object System.Collections.Generic.List[string]
                    for ($j = $i + 1; $j -lt $step.Count; $j++) {
                        if ($step[$j].Trim() -eq "" -or $step[$j].Length - $step[$j].TrimStart().Length -gt $runIndent) { $body.Add($step[$j]) } else { break }
                    }
                    $text = ($body -join "`n")
                } else { $text = $Matches[3] }
                # Expressions are replaced by a plain word: the workflow engine fills them in first.
                $text = [regex]::Replace($text, '\$\{\{.*?\}\}', 'X')
                Check "$($wf.Name) (a step with shell: powershell, #$number)" $text
            }
        }
    }
}
"check-powershell: $count scripts and steps, $bad syntax error(s), PowerShell $($PSVersionTable.PSVersion)"
if ($bad) { exit 1 }

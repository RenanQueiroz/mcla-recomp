# Installs the pinned Vulkan SDK on a CI runner (only its headers and import library are
# used, to build FSR's Vulkan runtime), after checking the installer against its SHA-256
# in scripts\ci\tools.json, and puts VULKAN_SDK in the environment of the later steps.
#   powershell -File scripts\ci\install-vulkan-sdk.ps1
$ErrorActionPreference = "Stop"
$tool = (Get-Content -LiteralPath "$PSScriptRoot\tools.json" -Raw | ConvertFrom-Json).'vulkan-sdk'
$temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
$installer = Join-Path $temp "vulkansdk-$($tool.version).exe"

# Downloaded up to three times (a CDN that fails once should not fail a 20-minute run); a file
# that does not match the pinned hash is deleted, and is not tried again as it is.
$ok = $false
for ($attempt = 1; $attempt -le 3 -and -not $ok; $attempt++) {
    curl.exe -sL --fail --proto "=https" --proto-redir "=https" -o $installer $tool.url
    if ($LASTEXITCODE -ne 0) { "download attempt $attempt failed ($LASTEXITCODE)"; Start-Sleep -Seconds (5 * $attempt); continue }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $installer).Hash.ToLowerInvariant()
    if ($actual -ne $tool.sha256) {
        Remove-Item -LiteralPath $installer -Force
        "attempt ${attempt}: the installer's SHA-256 is $actual, not the pinned $($tool.sha256)"
        Start-Sleep -Seconds (5 * $attempt)
        continue
    }
    $ok = $true
}
if (-not $ok) { throw "could not download the Vulkan SDK installer $($tool.url) with the pinned SHA-256" }
"Vulkan SDK $($tool.version): $([int]((Get-Item -LiteralPath $installer).Length / 1MB)) MB, SHA-256 checked"

# Only the core component: the headers, the loader's import library.
$install = Start-Process -FilePath $installer -Wait -PassThru -ArgumentList @(
    "--accept-licenses", "--default-answer", "--confirm-command", "install", "com.lunarg.vulkan.core")
"installer exit code: $($install.ExitCode)"
Remove-Item -LiteralPath $installer -Force
if ($install.ExitCode -ne 0) { throw "the Vulkan SDK installer failed ($($install.ExitCode))" }

$sdk = "C:\VulkanSDK\$($tool.version)"
foreach ($file in "Include\vulkan\vulkan.h", "Lib\vulkan-1.lib") {
    if (-not (Test-Path -LiteralPath "$sdk\$file")) { throw "$sdk\$file is missing after the installation" }
}
"VULKAN_SDK=$sdk"
if ($env:GITHUB_ENV) { "VULKAN_SDK=$sdk" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }
$global:LASTEXITCODE = 0

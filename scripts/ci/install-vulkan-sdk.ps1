# Installs the pinned Vulkan SDK on a CI runner (only its headers and import library are
# used, to build FSR's Vulkan runtime), after checking the installer against its SHA-256
# in scripts\ci\tools.json, and puts VULKAN_SDK in the environment of the later steps.
#   powershell -File scripts\ci\install-vulkan-sdk.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$tool = (Get-Content -LiteralPath "$PSScriptRoot\tools.json" -Raw | ConvertFrom-Json).'vulkan-sdk'
$temp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP }
$installer = Join-Path $temp "vulkansdk-$($tool.version).exe"

curl.exe -sL --fail --proto "=https" --proto-redir "=https" -o $installer $tool.url
if ($LASTEXITCODE -ne 0) { throw "could not download $($tool.url)" }
$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $installer).Hash.ToLowerInvariant()
if ($actual -ne $tool.sha256) {
    Remove-Item -LiteralPath $installer -Force
    throw "the Vulkan SDK installer's SHA-256 is $actual, not the pinned $($tool.sha256)"
}
"Vulkan SDK $($tool.version): $([int]((Get-Item -LiteralPath $installer).Length / 1MB)) MB, SHA-256 checked"

# Only the core component: the headers, the loader's import library.
$install = Start-Process -FilePath $installer -Wait -PassThru -ArgumentList @(
    "--accept-licenses", "--default-answer", "--confirm-command", "install", "com.lunarg.vulkan.core")
"installer exit code: $($install.ExitCode)"
Remove-Item -LiteralPath $installer -Force

$sdk = "C:\VulkanSDK\$($tool.version)"
foreach ($file in "Include\vulkan\vulkan.h", "Lib\vulkan-1.lib") {
    if (-not (Test-Path -LiteralPath "$sdk\$file")) { throw "$sdk\$file is missing after the installation" }
}
"VULKAN_SDK=$sdk"
if ($env:GITHUB_ENV) { "VULKAN_SDK=$sdk" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8 }

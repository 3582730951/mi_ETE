param(
    [string]$BuildServer = "build/server",
    [string]$BuildClientWin = "build/client/windows",
    [string]$AndroidDir = "client/android",
    [string]$Output = "dist/artifacts.zip",
    [string]$CertPath = "",
    [string]$CertPassword = ""
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$tmp = Join-Path $root "dist/package"
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue | Out-Null
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

function CopyIfExists($src, $dst) {
    if (Test-Path $src) {
        New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
        Copy-Item $src -Destination $dst -Recurse -Force
    }
}

$serverCandidates = @($BuildServer, "build/server")
$resolvedServer = $serverCandidates | Where-Object { Test-Path (Join-Path $root "$_/mi_server.exe") } | Select-Object -First 1
$clientCandidates = @($BuildClientWin, "build/client/windows", "build/client-win", "build/client")
$resolvedClient = $clientCandidates | Where-Object { Test-Path (Join-Path $root "$_/mi_client.exe") } | Select-Object -First 1

if (-not $resolvedServer) { Write-Host "[package] 未找到服务器可执行文件" -ForegroundColor Yellow }
if (-not $resolvedClient) { Write-Host "[package] 未找到 Windows 客户端可执行文件" -ForegroundColor Yellow }

if ($resolvedServer) {
    CopyIfExists (Join-Path $root "$resolvedServer/mi_server.exe") (Join-Path $tmp "server/mi_server.exe")
}
if ($resolvedClient) {
    CopyIfExists (Join-Path $root "$resolvedClient/mi_client.exe") (Join-Path $tmp "client/windows/mi_client.exe")
    CopyIfExists (Join-Path $root "$resolvedClient/mi_client_qt_ui.dll") (Join-Path $tmp "client/windows/mi_client_qt_ui.dll")
}
CopyIfExists (Join-Path $root "dist/win") (Join-Path $tmp "client/windows/dist")
CopyIfExists (Join-Path $root $AndroidDir) (Join-Path $tmp "client/android")
CopyIfExists (Join-Path $root "client/android/app/build/outputs/apk/debug/app-debug.apk") (Join-Path $tmp "client/android/app-debug.apk")

if ($CertPath -ne "" -and (Test-Path $CertPath)) {
    New-Item -ItemType Directory -Force -Path (Join-Path $tmp "certs") | Out-Null
    Copy-Item $CertPath -Destination (Join-Path $tmp "certs") -Force
    if ($CertPassword -ne "") {
        Set-Content -Path (Join-Path $tmp "certs/password.txt") -Value $CertPassword -NoNewline
    }
}

if (Test-Path $Output) { Remove-Item $Output -Force }
Compress-Archive -Path (Join-Path $tmp "*") -DestinationPath (Join-Path $root $Output)
Write-Host "[package] 打包完成 -> $Output"

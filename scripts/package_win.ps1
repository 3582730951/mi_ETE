param(
    [string]$BuildDir = "build/client/windows",
    [string]$QtDllDir = "",
    [string]$OutputDir = "dist/win",
    [string]$SignTool = "",
    [string]$CertPath = "",
    [string]$CertPassword = ""
)

$ErrorActionPreference = "Stop"

# 参数

$root = Split-Path -Parent $PSScriptRoot
$buildCandidates = @($BuildDir, "build/client/windows", "build/client-win", "build/client")
$resolvedBuild = $null
foreach ($c in $buildCandidates) {
    $candidate = Join-Path $root "$c/mi_client.exe"
    if (Test-Path $candidate) {
        $resolvedBuild = $c
        break
    }
}

if (-not $resolvedBuild) {
    Write-Host "[package] 未找到 mi_client.exe，请先构建客户端 (检查 build/* 路径)" -ForegroundColor Yellow
    exit 1
}

$clientExe = Join-Path $root "$resolvedBuild/mi_client.exe"
$qtDll = Join-Path $root "$resolvedBuild/mi_client_qt_ui.dll"

New-Item -ItemType Directory -Force -Path (Join-Path $root $OutputDir) | Out-Null
Copy-Item $clientExe -Destination (Join-Path $root $OutputDir) -Force

if (Test-Path $qtDll) {
    Copy-Item $qtDll -Destination (Join-Path $root $OutputDir) -Force
} elseif ($QtDllDir -ne "" -and (Test-Path $QtDllDir)) {
    Copy-Item (Join-Path $QtDllDir "mi_client_qt_ui.dll") -Destination (Join-Path $root $OutputDir) -Force -ErrorAction SilentlyContinue
}

# 部署 Qt 运行时，确保 UI 可直接运行
$qtUiPath = Join-Path $root $OutputDir "mi_client_qt_ui.dll"
if (Test-Path $qtUiPath) {
    $windeploy = Get-Command "windeployqt.exe" -ErrorAction SilentlyContinue
    if (-not $windeploy -and $env:QT_ROOT_DIR) {
        $candidate = Join-Path $env:QT_ROOT_DIR "bin/windeployqt.exe"
        if (Test-Path $candidate) { $windeploy = $candidate }
    }
    if ($windeploy) {
        & $windeploy --dir (Join-Path $root $OutputDir) --no-translations --release $qtUiPath
    } else {
        Write-Host "[package] 未找到 windeployqt.exe，Qt 运行时未自动拷贝，可能导致 UI 运行失败" -ForegroundColor Yellow
    }
}

# 可选复制 Qt 依赖目录
if ($QtDllDir -ne "") {
    $qtBin = Join-Path $QtDllDir "bin"
    if (Test-Path $qtBin) {
        Write-Host "[package] 复制 Qt 运行时到 vendor/qt"
        New-Item -ItemType Directory -Force -Path (Join-Path $root "$OutputDir/vendor/qt") | Out-Null
        Copy-Item "$qtBin/*.dll" -Destination (Join-Path $root "$OutputDir/vendor/qt") -Force -ErrorAction SilentlyContinue
    }
}

if ($SignTool -ne "" -and (Test-Path $SignTool) -and $CertPath -ne "" -and (Test-Path $CertPath)) {
    Write-Host "[package] 开始签名"
    $exePath = Join-Path $root $OutputDir "mi_client.exe"
    $dllPath = Join-Path $root $OutputDir "mi_client_qt_ui.dll"
    & $SignTool sign /f $CertPath /p $CertPassword /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $exePath
    if (Test-Path $dllPath) {
        & $SignTool sign /f $CertPath /p $CertPassword /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $dllPath
    }
}

Write-Host "[package] 输出目录: $(Join-Path $root $OutputDir)"

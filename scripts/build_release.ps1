<#
.SYNOPSIS
    ZeroBorders 发行版打包脚本
.DESCRIPTION
    1. 清理旧构建
    2. 以 Release 模式编译
    3. windeployqt 自动部署 Qt 运行时 DLL
    4. 收集可执行文件、DLL、图标等资源到 dist/ZeroBorders/
    5. 打包为 ZIP
.NOTES
    用法：在项目根目录执行
    pwsh -File scripts\build_release.ps1
    或在 PowerShell 中：
    .\scripts\build_release.ps1
#>

param(
    [string]$QtPath = "C:\Qt\Qt6.11.1\6.11.1\msvc2022_64",
    [string]$BuildDir = "$PSScriptRoot\..\build_release",
    [string]$DistDir  = "$PSScriptRoot\..\dist",
    [string]$Version  = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Resolve-Path "$PSScriptRoot\.."
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  ZeroBorders 发行版打包" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "项目根目录: $ProjectRoot"
Write-Host "Qt 路径:    $QtPath"
Write-Host "构建目录:   $BuildDir"
Write-Host "输出目录:   $DistDir"
Write-Host ""

# 1. 确定 Qt windeployqt 路径
$WinDeployQt = Join-Path $QtPath "bin\windeployqt.exe"
if (-not (Test-Path $WinDeployQt)) {
    Write-Host "错误: 找不到 windeployqt.exe: $WinDeployQt" -ForegroundColor Red
    Write-Host "请通过 -QtPath 参数指定正确的 Qt 安装路径" -ForegroundColor Yellow
    exit 1
}

# 2. 清理并重新配置 CMake
Write-Host "[1/5] 配置 CMake (Release)..." -ForegroundColor Yellow
if (Test-Path $BuildDir) {
    Remove-Item -Recurse -Force $BuildDir
}
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null

cmake -S $ProjectRoot -B $BuildDir `
    -G "Visual Studio 17 2022" `
    -A x64 `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_PREFIX_PATH=$QtPath
if ($LASTEXITCODE -ne 0) {
    Write-Host "CMake 配置失败" -ForegroundColor Red
    exit 1
}

# 3. 编译
Write-Host "[2/5] 编译 Release..." -ForegroundColor Yellow
cmake --build $BuildDir --config Release --target ZeroBorders --parallel
if ($LASTEXITCODE -ne 0) {
    Write-Host "编译失败" -ForegroundColor Red
    exit 1
}

# 4. windeployqt 部署
$ExePath = Join-Path $BuildDir "bin\Release\ZeroBorders.exe"
if (-not (Test-Path $ExePath)) {
    Write-Host "错误: 找不到编译产物: $ExePath" -ForegroundColor Red
    exit 1
}

Write-Host "[3/5] 运行 windeployqt 部署 Qt 运行时..." -ForegroundColor Yellow
& $WinDeployQt --release --no-translations --no-system-d3d-compiler `
    --no-opengl-sw --no-quick-import --no-svg `
    --compiler-runtime `
    $ExePath
if ($LASTEXITCODE -ne 0) {
    Write-Host "windeployqt 部署失败" -ForegroundColor Red
    exit 1
}

# 5. 收集到 dist 目录
Write-Host "[4/5] 收集文件到 dist 目录..." -ForegroundColor Yellow
if (Test-Path $DistDir) {
    Remove-Item -Recurse -Force $DistDir
}
$PackageDir = Join-Path $DistDir "ZeroBorders"
New-Item -ItemType Directory -Path $PackageDir -Force | Out-Null

# 复制 exe 及所有 DLL（windeployqt 已放在同目录）
$BinReleaseDir = Join-Path $BuildDir "bin\Release"
Copy-Item -Path "$BinReleaseDir\*" -Destination $PackageDir -Recurse -Force

# 创建 log 目录（程序运行时写入日志）
New-Item -ItemType Directory -Path "$PackageDir\log" -Force | Out-Null

# 6. 打包 ZIP
Write-Host "[5/5] 打包 ZIP..." -ForegroundColor Yellow

# 确定版本号
if ($Version -eq "") {
    $Version = Get-Date -Format "yyyyMMdd"
}
$ZipName = "ZeroBorders_$Version.zip"
$ZipPath = Join-Path $DistDir $ZipName

if (Test-Path $ZipPath) {
    Remove-Item -Force $ZipPath
}

Compress-Archive -Path "$PackageDir\*" -DestinationPath $ZipPath -CompressionLevel Optimal

Write-Host ""
Write-Host "========================================" -ForegroundColor Green
Write-Host "  打包完成!" -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host "打包目录: $PackageDir"
Write-Host "ZIP 文件: $ZipPath"
$fileSize = (Get-Item $ZipPath).Length / 1MB
Write-Host ("ZIP 大小: {0:N1} MB" -f $fileSize)
Write-Host ""

# 列出打包内容摘要
Write-Host "打包内容:" -ForegroundColor Cyan
Get-ChildItem $PackageDir -File | ForEach-Object {
    Write-Host "  $($_.Name)"
}
$dllCount = (Get-ChildItem $PackageDir -Filter "*.dll").Count
Write-Host "共 $dllCount 个 DLL"

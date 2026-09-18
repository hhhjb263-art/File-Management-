# 构建云匣正式客户端（Windows / Qt6 + MinGW）
# 用法（在 PowerShell 中执行）：
#   .\scripts\build_windows.ps1
#   .\scripts\build_windows.ps1 -Qt D:\Qt\6.9.3\mingw_64 -MinGW D:\Qt\Tools\mingw1310_64
#   .\scripts\build_windows.ps1 -Clean       # 先删除构建目录再全量编译
#
# 注意：本文件必须保存为「UTF-8 with BOM」——Windows PowerShell 5.1 会把无 BOM 的 UTF-8
# 当成 ANSI/GBK 读取，中文注释会被误解码并破坏语法（表现为莫名其妙的语法错误）。
param(
    [string]$Qt       = "D:\Qt\6.9.3\mingw_64",
    [string]$MinGW    = "D:\Qt\Tools\mingw1310_64",
    [string]$BuildDir = "build_official",
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bd   = Join-Path $root $BuildDir

if (-not (Test-Path "$Qt\bin\qmake.exe"))  { throw "未找到 $Qt\bin\qmake.exe" }
if (-not (Test-Path "$MinGW\bin\g++.exe")) { throw "未找到 $MinGW\bin\g++.exe" }

if ($Clean -and (Test-Path $bd)) { Remove-Item -Recurse -Force $bd }
New-Item -ItemType Directory -Force -Path $bd | Out-Null

# 关键：qmake 需要在 PATH 里找到 g++，否则报 "Cannot run compiler 'g++'"
$env:PATH = "$Qt\bin;$MinGW\bin;$env:PATH"

Push-Location $bd
try {
    Write-Host "==> qmake (生成 Makefile)" -ForegroundColor Cyan
    & "$Qt\bin\qmake.exe" "..\File.pro"
    if ($LASTEXITCODE -ne 0) { throw "qmake 失败 (RC=$LASTEXITCODE)" }

    Write-Host "==> mingw32-make -j4 (编译 + 链接)" -ForegroundColor Cyan
    & "$MinGW\bin\mingw32-make.exe" -j4
    if ($LASTEXITCODE -ne 0) { throw "make 失败 (RC=$LASTEXITCODE)" }
} finally { Pop-Location }

$exe = Join-Path $root "bin\CloudVault.exe"
if (Test-Path $exe) {
    $size = (Get-Item $exe).Length
    Write-Host ("构建成功: {0}  ({1:N0} 字节)" -f $exe, $size) -ForegroundColor Green
    Write-Host "首次运行/换机器请先部署 Qt 依赖: bash scripts/deploy_windows.sh"
} else {
    throw "构建结束但未找到 $exe"
}

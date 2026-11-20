# 安装 / 卸载当前用户的右键菜单项“用 openmd 打开”。
# 只写 HKCU，不需要管理员权限，也不修改默认文件关联。
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install-association.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install-association.ps1 -Uninstall
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install-association.ps1 -Exe "D:\apps\openmd.exe"
param(
    [string]$Exe = "",
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrEmpty($Exe)) { $Exe = Join-Path $root 'dist\openmd.exe' }
$key = 'HKCU\Software\Classes\*\shell\openmd'

if ($Uninstall) {
    & reg.exe delete $key /f 2>$null | Out-Null
    Write-Host '已移除右键菜单：用 openmd 打开'
    exit 0
}

if (!(Test-Path $Exe)) {
    throw "找不到 openmd.exe：$Exe`n请先运行 build.bat，或用 -Exe 指定 exe 路径。"
}
$exePath = (Resolve-Path $Exe).Path
$command = '"' + $exePath + '" "%1"'

& reg.exe add $key /ve /d '用 openmd 打开' /f | Out-Null
& reg.exe add $key /v Icon /d ($exePath + ',0') /f | Out-Null
& reg.exe add ($key + '\command') /ve /d $command /f | Out-Null

Write-Host '已安装右键菜单：用 openmd 打开'
Write-Host ("  exe : " + $exePath)
Write-Host '  范围: 当前用户（HKCU），对所有文件生效'
Write-Host '如需卸载，运行：'
Write-Host '  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install-association.ps1 -Uninstall'

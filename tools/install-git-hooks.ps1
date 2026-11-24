# 安装仓库内置 Git 钩子（Windows / PowerShell）
#   用法：powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\install-git-hooks.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $root
try {
    git config --local core.hooksPath .githooks
    if ($LASTEXITCODE -ne 0) { throw 'git config core.hooksPath 失败' }

    $name  = git config user.name
    $email = git config user.email
    Write-Host "core.hooksPath = .githooks"
    Write-Host "user.name      = $name"
    Write-Host "user.email     = $email"

    if ([string]::IsNullOrWhiteSpace($email) -or $email -like '*+Shenyun-Z@users.noreply.github.com') {
        Write-Warning '当前身份会被 pre-commit 钩子拦截。请先改正：'
        Write-Host '  git config --local user.name  "whylantern"'
        Write-Host '  git config --local user.email "3778216901@qq.com"'
    }
    else {
        Write-Host '钩子已生效，后续提交不会记到他人名下。'
    }
}
finally { Pop-Location }

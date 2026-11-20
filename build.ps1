# Builds dist\openmd.exe with a MinGW-w64 C++ compiler.
# No .NET Framework, SDK, NuGet, or network access is required at runtime.
param([string]$Cxx = "")
$ErrorActionPreference = 'Continue'

$root = $PSScriptRoot
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'

New-Item -ItemType Directory -Force -Path $build | Out-Null

# Keep previous builds rather than deleting them. Archiving happens only after
# a successful link, so a failed build never destroys the last working dist.
# `.old/` is intentionally hidden/git-ignored.
function Archive-Dist {
    if (!(Test-Path -LiteralPath $dist)) { return }
    $archiveRoot = Join-Path $root '.old'
    New-Item -ItemType Directory -Force -Path $archiveRoot | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $archived = Join-Path $archiveRoot ("dist-$stamp")
    $suffix = 1
    while (Test-Path -LiteralPath $archived) {
        $archived = Join-Path $archiveRoot ("dist-$stamp-$suffix")
        $suffix++
    }
    Move-Item -LiteralPath $dist -Destination $archived
    Write-Host "archived previous dist -> $archived"
    Prune-Archives
}

# Keep only the newest backup for each FileVersion. This keeps one rollback
# point per released version instead of every intermediate build.
function Prune-Archives {
    $archiveRoot = Join-Path $root '.old'
    if (!(Test-Path -LiteralPath $archiveRoot)) { return }
    $groups = @{}
    $empty = @()
    foreach ($dir in Get-ChildItem -LiteralPath $archiveRoot -Directory) {
        $exe = Join-Path $dir.FullName 'openmd.exe'
        if (!(Test-Path -LiteralPath $exe)) {
            if (@(Get-ChildItem -LiteralPath $dir.FullName -Force).Count -eq 0) {
                $empty += $dir
            }
            continue
        }
        $version = ''
        try {
            $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($exe).FileVersion
        } catch { }
        if (!$version) { $version = 'unknown' }
        if (!$groups.ContainsKey($version)) { $groups[$version] = @() }
        $groups[$version] += $dir
    }
    foreach ($version in $groups.Keys) {
        $list = @($groups[$version] | Sort-Object Name -Descending)
        for ($i = 1; $i -lt $list.Count; $i++) {
            Remove-Item -LiteralPath $list[$i].FullName -Recurse -Force
            Write-Host ("pruned backup " + $version + " -> " + $list[$i].Name)
        }
    }
    foreach ($dir in $empty) {
        Remove-Item -LiteralPath $dir.FullName -Recurse -Force
        Write-Host ("pruned empty backup -> " + $dir.Name)
    }
}

function Find-Tool([string]$name) {
    # Prefer the compiler vendored inside the project, so a fresh checkout can
    # build without installing MinGW separately.
    $candidates = @(
        (Join-Path $root "tools\w64devkit\bin\$name.exe"),
        (Join-Path $root "tools\mingw64\bin\$name.exe"),
        (Join-Path $root "tools\mingw32\bin\$name.exe"),
        "$env:LOCALAPPDATA\openmd-build\w64devkit\bin\$name.exe",
        "C:\msys64\mingw64\bin\$name.exe",
        "C:\mingw64\bin\$name.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$cxx = $Cxx
if (-not $cxx) { $cxx = $env:OPENMD_CXX }
if (-not $cxx) { $cxx = Find-Tool 'g++' }
$gcc = Find-Tool 'gcc'
$windres = Find-Tool 'windres'
if ($cxx) {
    if (-not $gcc) { $candidate = Join-Path (Split-Path $cxx) 'gcc.exe'; if (Test-Path $candidate) { $gcc = $candidate } }
    if (-not $windres) { $candidate = Join-Path (Split-Path $cxx) 'windres.exe'; if (Test-Path $candidate) { $windres = $candidate } }
}
if (-not $cxx -or -not $gcc -or -not $windres) {
    Write-Host ''
    Write-Host 'openmd 需要 MinGW-w64 工具链（g++ / gcc / windres）。' -ForegroundColor Yellow
    Write-Host '推荐下载免安装的 w64devkit：https://github.com/skeeto/w64devkit/releases'
    Write-Host '解压后把 bin 目录加入 PATH，或把 w64devkit 放到 tools\w64devkit。'
    Write-Host '也可以用 -Cxx <g++.exe 路径> 指定编译器。'
    Write-Host ''
    exit 1
}

$defs = @('-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00')
$common = @('-std=c++20', '-finput-charset=UTF-8') + $defs
$objects = @()

Write-Host 'compiling miniz...'
$minizLog = & $gcc @('-O2') @defs -c (Join-Path $root 'vendor\miniz.c') -o (Join-Path $build 'miniz.o') 2>&1
if ($LASTEXITCODE -ne 0 -or !(Test-Path (Join-Path $build 'miniz.o'))) {
    $minizLog | Select-Object -Last 20
    throw 'miniz compile failed'
}
$objects += Join-Path $build 'miniz.o'

$sources = Get-ChildItem (Join-Path $root 'src\*.cpp') | Sort-Object Name
foreach ($src in $sources) {
    $obj = Join-Path $build ($src.BaseName + '.o')
    Write-Host ("CC  " + $src.Name)
    # Default to O2 for every file. GCC on this machine occasionally crashes
    # (ICE) at O2 in a different function/pass each run, and immediately
    # succeeds on retry. That is compiler/CPU instability, not a code defect.
    # Retry O2 many times, then try O2 with RTL block reordering disabled
    # (still fully optimized), and only then demote to O1/O0.
    $attempts = @(
        @{ Opt = '-O2'; Extra = @(); Label = '-O2' },
        @{ Opt = '-O2'; Extra = @(); Label = '-O2' },
        @{ Opt = '-O2'; Extra = @(); Label = '-O2' },
        @{ Opt = '-O2'; Extra = @(); Label = '-O2' },
        @{ Opt = '-O2'; Extra = @(); Label = '-O2' },
        @{ Opt = '-O2'; Extra = @(); Label = '-O2' },
        @{ Opt = '-O2'; Extra = @('-fno-reorder-blocks', '-fno-reorder-blocks-and-partition'); Label = '-O2 no-reorder' },
        @{ Opt = '-O2'; Extra = @('-fno-reorder-blocks', '-fno-reorder-blocks-and-partition'); Label = '-O2 no-reorder' },
        @{ Opt = '-O1'; Extra = @(); Label = '-O1' },
        @{ Opt = '-O0'; Extra = @(); Label = '-O0' }
    )
    $built = $false
    foreach ($attempt in $attempts) {
        Remove-Item -LiteralPath $obj -Force -ErrorAction SilentlyContinue
        $opt = $attempt.Opt
        $extra = $attempt.Extra
        $label = $attempt.Label
        $compileLog = & $cxx @common $opt @extra -c $src.FullName -o $obj 2>&1
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $obj)) {
            $built = $true
            if ($label -ne '-O2') {
                Write-Host ("    " + $src.Name + " built with " + $label)
            }
            break
        }
        Write-Host ("    retrying " + $src.Name + " at " + $label)
        if ($attempt -eq $attempts[-1]) {
            $compileLog | Select-Object -Last 12 | ForEach-Object { Write-Host $_ }
        }
    }
    if (-not $built) { throw "compile failed: $($src.Name)" }
    $objects += $obj
}

if (!(Test-Path (Join-Path $root 'vendor\pdfium\pdfium.dll'))) {
    throw '缺少 vendor\pdfium\pdfium.dll（PDF 引擎）。请保留 vendor/pdfium 目录。'
}

Write-Host 'compiling resources...'
$res = Join-Path $build 'openmd_res.o'
Push-Location $root
& $windres -i 'res\openmd.rc' -o $res -I '.'
$resCode = $LASTEXITCODE
Pop-Location
if ($resCode -ne 0) { throw 'windres failed' }
$objects += $res

$tmpExe = Join-Path $build 'openmd.exe'
$libs = @(
    '-ld2d1', '-ldwrite', '-lwindowscodecs', '-lole32', '-loleaut32', '-luuid',
    '-lshell32', '-lshlwapi', '-lcomdlg32', '-lgdi32', '-luser32', '-ldwmapi',
    '-ladvapi32', '-lcomctl32', '-luxtheme'
)
Write-Host 'linking...'
Remove-Item -LiteralPath $tmpExe -Force -ErrorAction SilentlyContinue
& $cxx @common '-mwindows' '-municode' '-static' '-static-libgcc' '-static-libstdc++' '-s' `
    -o $tmpExe @objects @libs
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $tmpExe)) { throw 'link failed' }

# Only replace dist after a successful build. The previous dist is archived
# under .old/; a failed compile/link leaves it untouched.
Archive-Dist
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$out = Join-Path $dist 'openmd.exe'
Move-Item -LiteralPath $tmpExe -Destination $out

$size = (Get-Item $out).Length
Write-Host ''
Write-Host ("OK  ->  {0}  ({1:N0} KB)" -f $out, ($size / 1KB)) -ForegroundColor Green

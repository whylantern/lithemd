param([string]$Cxx = "")
$ErrorActionPreference = 'Continue'

$root = $PSScriptRoot
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $build | Out-Null

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
}

function Prune-Archives {
    $archiveRoot = Join-Path $root '.old'
    if (!(Test-Path -LiteralPath $archiveRoot)) { return }
    $groups = @{}
    foreach ($dir in Get-ChildItem -LiteralPath $archiveRoot -Directory) {
        $exe = Join-Path $dir.FullName 'lithemd.exe'
        if (!(Test-Path -LiteralPath $exe)) { $exe = Join-Path $dir.FullName 'openmd.exe' }
        if (!(Test-Path -LiteralPath $exe)) { continue }
        $version = ''
        try { $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($exe).FileVersion } catch { }
        if (!$version) { $version = 'unknown' }
        if (!$groups.ContainsKey($version)) { $groups[$version] = @() }
        $groups[$version] += $dir
    }
    foreach ($version in $groups.Keys) {
        $list = @($groups[$version] | Sort-Object LastWriteTime -Descending)
        for ($i = 1; $i -lt $list.Count; $i++) {
            Remove-Item -LiteralPath $list[$i].FullName -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Find-Tool([string]$name) {
    $candidates = @(
        (Join-Path $root "tools\w64devkit\bin\$name.exe"),
        (Join-Path $root "tools\mingw64\bin\$name.exe"),
        (Join-Path $root "tools\mingw32\bin\$name.exe"),
        "$env:LOCALAPPDATA\lithemd-build\w64devkit\bin\$name.exe",
        "C:\msys64\mingw64\bin\$name.exe",
        "C:\mingw64\bin\$name.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return $candidate }
    }
    $command = Get-Command $name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

$cxx = $Cxx
if (-not $cxx) { $cxx = $env:LITHEMD_CXX }
if (-not $cxx) { $cxx = $env:OPENMD_CXX }
if (-not $cxx) { $cxx = Find-Tool 'g++' }
$gcc = Find-Tool 'gcc'
$windres = Find-Tool 'windres'
if (-not $cxx -or -not $gcc -or -not $windres) {
    Write-Host 'LitheMD requires MinGW-w64 g++, gcc and windres.' -ForegroundColor Yellow
    exit 1
}

$defs = @('-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00',
          '-ffunction-sections', '-fdata-sections')
$common = @('-std=c++20', '-finput-charset=UTF-8') + $defs

Write-Host 'compiling miniz...'
$minizObject = Join-Path $build 'miniz.o'
$minizBuilt = $false
foreach ($opt in @(@('-O2',@()), @('-O2',@('-fno-reorder-blocks','-fno-reorder-blocks-and-partition')), @('-O1',@()), @('-O0',@()))) {
    Remove-Item -LiteralPath $minizObject -Force -ErrorAction SilentlyContinue
    & $gcc @($opt[0]) @($opt[1]) @defs -c (Join-Path $root 'vendor\miniz.c') -o $minizObject 2>&1 | Out-Host
    if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $minizObject)) { $minizBuilt = $true; break }
    Write-Host ("    retrying miniz at " + $opt[0])
}
if (-not $minizBuilt) { throw 'miniz compile failed' }

$objects = @($minizObject)
$sources = Get-ChildItem (Join-Path $root 'src\*.cpp') | Sort-Object Name
if ($sources.Count -eq 0) { throw 'no source files found' }
foreach ($src in $sources) {
    $obj = Join-Path $build ($src.BaseName + '.o')
    Write-Host ("CC  " + $src.Name)
    $attempts = @(
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
        $compileLog = & $cxx @common $attempt.Opt @($attempt.Extra) -c $src.FullName -o $obj 2>&1
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $obj)) {
            $built = $true
            if ($attempt.Label -ne '-O2') { Write-Host ("    " + $src.Name + " built with " + $attempt.Label) }
            break
        }
        Write-Host ("    retrying " + $src.Name + " at " + $attempt.Label)
    }
    if (-not $built) { throw "compile failed: $($src.Name)" }
    $objects += $obj
}

Write-Host 'compiling resources...'
$resObj = Join-Path $build 'lithemd_res.o'
& $windres -i 'res\lithemd.rc' -o $resObj -I '.'
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $resObj)) { throw 'windres failed' }
$objects += [string]$resObj

$tmpExe = Join-Path $build 'lithemd.exe'
$libs = @(
    '-ld2d1', '-ldwrite', '-lwindowscodecs', '-lole32', '-loleaut32', '-luuid',
    '-lshell32', '-lshlwapi', '-lcomdlg32', '-lgdi32', '-luser32', '-ldwmapi',
    '-ladvapi32', '-lcomctl32', '-luxtheme', '-lbcrypt', '-lwinmm'
)
Write-Host 'linking...'
Remove-Item -LiteralPath $tmpExe -Force -ErrorAction SilentlyContinue
& $cxx @common '-mwindows' '-municode' '-static' '-static-libgcc' '-static-libstdc++' '-s' '-Wl,--gc-sections' -o $tmpExe @objects @libs
if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $tmpExe)) { throw 'link failed' }

Archive-Dist
Prune-Archives
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$out = Join-Path $dist 'lithemd.exe'
Move-Item -LiteralPath $tmpExe -Destination $out
$size = (Get-Item $out).Length
Write-Host ''
Write-Host ("OK  ->  {0}  ({1:N0} KB)" -f $out, ($size / 1KB)) -ForegroundColor Green

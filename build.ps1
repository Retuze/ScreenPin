#requires -Version 7.0
<#
.SYNOPSIS
    Build ScreenPin with the native MSVC toolchain (cl.exe).

.DESCRIPTION
    Requirements:
      * Visual Studio 2022 Build Tools with the "Desktop development with C++"
        workload (MSVC + Windows SDK)

    The script discovers Visual Studio through vswhere and uses the environment
    created by vcvars64.bat.  It does not contain machine-specific paths.

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Clean
#>
[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Set-Location $root

$buildDir = Join-Path $root 'build'
$distDir = Join-Path $root 'dist'
$srcDir = Join-Path $root 'src'
$resourceDir = Join-Path $root 'resources'

if ($Clean) {
    foreach ($dir in @($buildDir, $distDir)) {
        if (Test-Path $dir) {
            try {
                Remove-Item $dir -Recurse -Force
            }
            catch {
                throw "Cannot clean '$dir'. Close ScreenPin before rebuilding."
            }
        }
    }
}

# --- locate Visual Studio / vcvars -----------------------------------------
$programFilesX86 = ${env:ProgramFiles(x86)}
if (-not $programFilesX86) { $programFilesX86 = $env:ProgramFiles }
$vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'vswhere.exe not found. Install Visual Studio 2022 Build Tools.'
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1
if (-not $vsPath) {
    throw 'MSVC C++ tools not found. Install the "Desktop development with C++" workload.'
}

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at '$vcvars'."
}

# --- compile and link ------------------------------------------------------
New-Item -ItemType Directory -Force -Path $buildDir, $distDir | Out-Null

$sourceList = @(
    'main.cpp',
    'app.cpp',
    'common.cpp',
    'overlay.cpp',
    'pin_window.cpp'
)
$sourceArgs = ($sourceList | ForEach-Object {
    '"' + (Join-Path $srcDir $_) + '"'
}) -join ' '

$command = "call `"$vcvars`" >nul && " +
    "cd /d `"$root`" && rc /nologo /I resources /fo `"$buildDir\app.res`" resources\app.rc && " +
    "cd /d `"$buildDir`" && " +
    "cl /nologo /std:c++latest /O2 /EHsc /W4 /MT /utf-8 " +
    "/DUNICODE /D_UNICODE /I `"$srcDir`" /I `"$resourceDir`" " +
    "$sourceArgs app.res /Fe:ScreenPin.exe /link " +
    "/SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup " +
    "user32.lib gdi32.lib shell32.lib comdlg32.lib gdiplus.lib ole32.lib " +
    "advapi32.lib"

Write-Host 'Building ScreenPin (cl.exe + Windows SDK)...' -ForegroundColor Cyan
& cmd.exe /c $command
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

$artifact = Join-Path $buildDir 'ScreenPin.exe'
Copy-Item $artifact (Join-Path $distDir 'ScreenPin.exe') -Force

$size = (Get-Item (Join-Path $distDir 'ScreenPin.exe')).Length
Write-Host ("Done: dist\ScreenPin.exe ({0:N0} bytes)" -f $size) -ForegroundColor Green

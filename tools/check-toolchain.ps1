<#
.SYNOPSIS
  Checks that the tools needed to build DSP Toolbox are installed.
.DESCRIPTION
  Prints a pass/fail table for: Visual Studio 2022 (VC tools), CMake >= 3.28, Ninja, Git, vcpkg.
  Exit code 0 if everything passes, 1 otherwise. Never installs anything.
  A tool counts as present if it is on PATH or bundled with Visual Studio (CMake and Ninja ship with the
  "Desktop development with C++" workload).
#>
$ErrorActionPreference = 'Continue'
$results = @()

function Add-Result($name, $ok, $detail) {
    $script:results += [pscustomobject]@{ Tool = $name; Status = $(if ($ok) { 'PASS' } else { 'FAIL' }); Detail = $detail }
}

# --- Visual Studio 2022 with VC tools ---
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -version '[17.0,18.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
}
Add-Result 'Visual Studio 2022 + VC tools' ([bool]$vsPath) $(if ($vsPath) { $vsPath } else { 'not found (install "Desktop development with C++")' })

# --- CMake >= 3.28 ---
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake -and $vsPath) {
    $p = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path $p) { $cmake = Get-Item $p }
}
if ($cmake) {
    $exe = if ($cmake.Source) { $cmake.Source } else { $cmake.FullName }
    $ver = ((& $exe --version | Select-Object -First 1) -replace '[^0-9.]', '')
    $ok = ([version]$ver -ge [version]'3.28')
    Add-Result 'CMake >= 3.28' $ok "$ver ($exe)"
} else { Add-Result 'CMake >= 3.28' $false 'not found' }

# --- Ninja ---
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninja -and $vsPath) {
    $p = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
    if (Test-Path $p) { $ninja = Get-Item $p }
}
if ($ninja) {
    $exe = if ($ninja.Source) { $ninja.Source } else { $ninja.FullName }
    Add-Result 'Ninja' $true "$(& $exe --version) ($exe)"
} else { Add-Result 'Ninja' $false 'not found' }

# --- Git ---
$git = Get-Command git -ErrorAction SilentlyContinue
Add-Result 'Git' ([bool]$git) $(if ($git) { (& git --version) } else { 'not found' })

# --- vcpkg ---
$vcpkgRoot = $env:VCPKG_ROOT
$vcpkgOk = $vcpkgRoot -and (Test-Path (Join-Path $vcpkgRoot 'vcpkg.exe'))
$detail = if ($vcpkgOk) { $vcpkgRoot }
          elseif ($vcpkgRoot) { "VCPKG_ROOT=$vcpkgRoot but vcpkg.exe is missing (run bootstrap-vcpkg.bat)" }
          else { 'VCPKG_ROOT not set (clone https://github.com/microsoft/vcpkg and bootstrap, e.g. C:\dev\vcpkg)' }
Add-Result 'vcpkg (VCPKG_ROOT)' $vcpkgOk $detail

$results | Format-Table -AutoSize -Wrap
if ($results.Status -contains 'FAIL') { Write-Host 'Toolchain check FAILED.' -ForegroundColor Red; exit 1 }
Write-Host 'Toolchain check passed.' -ForegroundColor Green
exit 0

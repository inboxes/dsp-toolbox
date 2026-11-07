<#
.SYNOPSIS
  Loads the MSVC x64 build environment (vcvars64.bat) into the current PowerShell session, and puts the
  CMake/Ninja that the check-toolchain script found on PATH. Dot-source it:  . .\tools\dev-env.ps1
.DESCRIPTION
  CMake presets cannot run vcvars64.bat themselves, so run this once per shell before `cmake --preset ...`
  (or just use an "x64 Native Tools Command Prompt"). It is a no-op when cl.exe is already on PATH.
#>
if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -version '[17.0,18.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (-not $vs) { throw 'Visual Studio 2022 with the C++ workload not found. Run tools/check-toolchain.ps1.' }
$bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

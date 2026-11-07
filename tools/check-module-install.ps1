<#
.SYNOPSIS
  Proves a separately released module works: installs the packaged app to a temp folder, unzips the module into a
  temp user data folder, validates it and runs the app's smoke test with it.
#>
param(
    [Parameter(Mandatory)][string]$Zip,
    [Parameter(Mandatory)][string]$Id,
    [Parameter(Mandatory)][string]$BuildDir
)
$ErrorActionPreference = 'Stop'

$temp = Join-Path ([IO.Path]::GetTempPath()) ("dsptb-module-" + [guid]::NewGuid().ToString('N'))
$app = Join-Path $temp 'app'
$data = Join-Path $temp 'data'
try {
    & cmake --install $BuildDir --component app --prefix $app | Out-Null
    if ($LASTEXITCODE) { throw 'cmake --install failed' }
    if (Test-Path (Join-Path $app "modules/$Id")) { throw "$Id is in the app package, so this test would prove nothing." }

    $modules = Join-Path $data 'modules'
    New-Item -ItemType Directory -Force $modules | Out-Null
    Expand-Archive $Zip $modules
    $package = Join-Path $modules $Id
    if (-not (Test-Path (Join-Path $package 'module.json'))) { throw "The zip has no top folder named $Id." }
    if (-not (Test-Path (Join-Path $package 'LICENSE'))) { throw 'The zip has no LICENSE file.' }

    & (Join-Path $app 'dsptb-cli.exe') validate $package
    if ($LASTEXITCODE) { throw "dsptb-cli validate failed with exit code $LASTEXITCODE" }

    $env:DSPTB_DATA_DIR = $data
    $p = Start-Process (Join-Path $app 'DSPToolbox.exe') -ArgumentList '--smoke-test', '300', '--null-audio' -PassThru -Wait
    $log = Join-Path $data 'logs/dsptb.log'
    if ($p.ExitCode -ne 0) { Get-Content $log; throw "The smoke test failed with exit code $($p.ExitCode)" }
    if (-not (Select-String -Path $log -SimpleMatch 'Found 4 module package(s)' -Quiet)) {
        Get-Content $log
        throw "The app didn't find the module in the user data folder."
    }
    Write-Host "Module OK: $Id installs from its zip, validates and loads in the packaged app"
} finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}

<#
.SYNOPSIS
  Downloads the Steinberg ASIO SDK into third_party/asio-sdk/ (ignored by git, never committed).
.DESCRIPTION
  The SDK is used under its GPLv3 option (the project is GPLv3). Build with
  -DDSPTB_ENABLE_ASIO=ON afterwards; DSPTB_ASIO_SDK_DIR defaults to third_party/asio-sdk/ASIOSDK.
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root 'third_party\asio-sdk'
$zip = Join-Path $env:TEMP 'asiosdk.zip'
if (Test-Path (Join-Path $dest 'ASIOSDK\common\iasiodrv.h')) {
    Write-Host "ASIO SDK already present in $dest"
    exit 0
}
New-Item -ItemType Directory -Force $dest | Out-Null
Write-Host 'Downloading the ASIO SDK from steinberg.net ...'
Invoke-WebRequest -Uri 'https://www.steinberg.net/asiosdk' -OutFile $zip
# Pinned like the Steam Audio download: a changed file must be reviewed (licence, API) before it is used.
$expected = 'D5EBF0C20DD2C5F43771FD0C1418F4B361BF52434EE670097CFA6B3A335E2ECA' # ASIO SDK 2.3.4 (2025-10-15)
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Write-Host "SHA-256: $hash"
if ($hash -ne $expected) {
    Remove-Item $zip
    throw "The downloaded SDK does not match the pinned hash $expected. Steinberg may have published a new version: check it, then update the hash in this script."
}
Expand-Archive $zip -DestinationPath $dest -Force
Remove-Item $zip
if (-not (Test-Path (Join-Path $dest 'ASIOSDK\common\iasiodrv.h'))) { throw 'Unexpected SDK layout: common\iasiodrv.h is missing.' }
Write-Host "Done. Configure with: cmake --preset windows-debug -DDSPTB_ENABLE_ASIO=ON"

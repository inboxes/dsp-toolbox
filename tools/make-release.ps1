<#
.SYNOPSIS
  Builds the release zips in dist/ (docs/RELEASING.md). Publishes nothing.
.EXAMPLE
  tools/make-release.ps1 -Version 0.1.0-rc.1
.EXAMPLE
  tools/make-release.ps1 -Version 0.1.0 -Module org.dsptb.spatializer -Repo owner/name
.PARAMETER Module
  Release one separately distributed module instead of the app. Also writes its checksum, catalog entry and source.
.PARAMETER Repo
  owner/name of the GitHub repo that will host a -Module release. Defaults to $env:GITHUB_REPOSITORY.
.PARAMETER AllowDirty
  Rehearsals only: accept uncommitted changes. The source zip still holds the committed state.
#>
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$')][string]$Version,
    [string]$Preset = 'windows-release-asio',
    [string]$OutDir = 'dist',
    [string]$Module = '',
    [string]$Repo = $env:GITHUB_REPOSITORY,
    [switch]$SkipBuild,
    [switch]$AllowDirty
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

# No named parameters: PowerShell would bind an argument such as cmake's -E to a parameter called -exe.
function Invoke-Checked {
    $exe, $rest = $args
    & $exe @rest
    if ($LASTEXITCODE) { throw "$($args -join ' ') failed with exit code $LASTEXITCODE" }
}

# Zips the contents of $folder. cmake's zip writes forward slashes, which every unzip tool expects.
function New-Zip([string]$folder, [string]$zip) {
    if (Test-Path $zip) { Remove-Item $zip }
    Push-Location $folder
    $tarArgs = @('-E', 'tar', 'cf', $zip, '--format=zip', '--') + @(Get-ChildItem -Name)
    try { Invoke-Checked cmake @tarArgs }
    finally { Pop-Location }
    Write-Host ("  {0} ({1:N1} MB)" -f (Split-Path $zip -Leaf), ((Get-Item $zip).Length / 1MB))
}

# The manifests of modules marked DISTRIBUTION BUNDLED. This works before anything is configured.
function Get-BundledManifests {
    foreach ($dir in Get-ChildItem modules -Directory) {
        $cmake = Join-Path $dir.FullName 'CMakeLists.txt'
        $manifest = Join-Path $dir.FullName 'module.json'
        if ((Test-Path $manifest) -and (Get-Content $cmake -Raw) -match 'DISTRIBUTION\s+BUNDLED') { $manifest }
    }
}

# --- Every version source must agree with the requested one ---
$base = $Version -replace '-.*$', ''
$mismatches = @()
$moduleManifest = ''
if ($Module) {
    $moduleManifest = Get-ChildItem modules/*/module.json | Where-Object {
        (Get-Content $_.FullName -Raw | ConvertFrom-Json).id -eq $Module
    } | Select-Object -First 1
    if (-not $moduleManifest) { throw "No first-party module has the id $Module." }
    if (Get-BundledManifests | Where-Object { $_ -eq $moduleManifest.FullName }) {
        throw "$Module is bundled, so it is released with the app."
    }
    if (-not $Repo) { throw 'A module release needs -Repo owner/name for its download URL.' }
    $moduleVersion = (Get-Content $moduleManifest.FullName -Raw | ConvertFrom-Json).version
    if ($moduleVersion -ne $base) { $mismatches += "$($moduleManifest.FullName) has $moduleVersion" }
} else {
    $cmakeVersion = [regex]::Match((Get-Content CMakeLists.txt -Raw), 'project\(dsptb VERSION (\S+)').Groups[1].Value
    if ($cmakeVersion -ne $base) { $mismatches += "CMakeLists.txt has $cmakeVersion" }
    $vcpkgVersion = (Get-Content vcpkg.json -Raw | ConvertFrom-Json).version
    if ($vcpkgVersion -ne $base) { $mismatches += "vcpkg.json has $vcpkgVersion" }
    foreach ($m in Get-BundledManifests) {
        $moduleVersion = (Get-Content $m -Raw | ConvertFrom-Json).version
        if ($moduleVersion -ne $base) { $mismatches += "$m has $moduleVersion" }
    }
}
if ($mismatches) { throw "Version $base doesn't match:`n$($mismatches -join "`n")" }

$dirty = git status --porcelain --untracked-files=no
if ($dirty -and -not $AllowDirty) { throw "Uncommitted changes:`n$($dirty -join "`n")" }

$build = Join-Path $root "build/$Preset"
$sofaBuild = Join-Path $root 'build/sofa-tool'
$asioSdk = Join-Path $root 'third_party/asio-sdk/ASIOSDK'
$triplet = 'x64-windows-static-md'

if (-not $SkipBuild) {
    Invoke-Checked powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch-asio-sdk.ps1
    Invoke-Checked cmake --preset $Preset
    Invoke-Checked cmake --build --preset $Preset
    Invoke-Checked cmake -S tools/sofa -B $sofaBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl `
        -DCMAKE_CXX_COMPILER=cl "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
        "-DVCPKG_TARGET_TRIPLET=$triplet"
    Invoke-Checked cmake --build $sofaBuild
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
$dist = (Resolve-Path $OutDir).Path
$stage = Join-Path $build 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }

function Install-Component([string]$component, [string]$prefix) {
    Invoke-Checked cmake --install $build --component $component --prefix $prefix
}

# --- Per-module zips (the package folder is the zip's single top folder, so Install from .zip finds it) ---
$separate = @(Get-Content (Join-Path $build 'packaging/separate-modules.txt') | Where-Object { $_ })
$moduleDirs = Get-ChildItem (Join-Path $build 'bin/modules') -Directory
if ($Module) {
    if ($separate -notcontains $Module) { throw "$Module has no separate release (see DISTRIBUTION in its CMakeLists.txt)." }
    $moduleDirs = @($moduleDirs | Where-Object Name -eq $Module)
} else {
    $moduleDirs = @($moduleDirs | Where-Object { $separate -notcontains $_.Name })
}
if (-not $moduleDirs) { throw "No module folder found in $build for: $Module" }
Write-Host 'Packaging:'
foreach ($dir in $moduleDirs) {
    $s = Join-Path $stage "module/$($dir.Name)"
    Install-Component "module_$($dir.Name)" $s
    New-Zip $s (Join-Path $dist "$($dir.Name)-$Version.zip")
}
if ($Module) {
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $zipName = "$Module-$Version.zip"
    $zip = Join-Path $dist $zipName
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    [IO.File]::WriteAllText("$zip.sha256", "$hash  $zipName`n", $utf8)

    # This version's catalog entry. The short name is the last part of the id, so tags stay predictable.
    $manifest = Get-Content $moduleManifest.FullName -Raw | ConvertFrom-Json
    $short = ($Module -split '\.')[-1]
    $entry = [ordered]@{
        id             = $Module
        name           = $manifest.name
        author         = $manifest.author
        description    = $manifest.description
        license        = $manifest.license
        homepage       = "https://github.com/$Repo"
        kinds          = @($manifest.modules | ForEach-Object kind | Sort-Object -Unique)
        version        = $Version
        abi            = $manifest.abi
        minHostVersion = $manifest.minHostVersion
        url            = "https://github.com/$Repo/releases/download/module/$short/v$Version/$zipName"
        sha256         = $hash
        size           = (Get-Item $zip).Length
        released       = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd')
    }
    [IO.File]::WriteAllText((Join-Path $dist "$Module-$Version.entry.json"), (($entry | ConvertTo-Json -Depth 4) + "`n"), $utf8)

    # GPLv3 source for exactly this release.
    Invoke-Checked git archive --format=zip "--prefix=$Module-$Version-source/" -o (Join-Path $dist "$Module-$Version-source.zip") HEAD
    Write-Host "Done: $dist"
    return
}

# --- Notices, shared by the app and SDK zips ---
$notices = Join-Path $build 'packaging/THIRD_PARTY_NOTICES.md'
& "$PSScriptRoot/generate-notices.ps1" `
    -ShareDirs "$build/vcpkg_installed/$triplet/share", "$sofaBuild/vcpkg_installed/$triplet/share" `
    -OutFile $notices -AsioSdkDir $asioSdk

# --- App ---
$appName = "DSPToolbox-$Version-win64"
$app = Join-Path $stage "app/$appName"
Install-Component app $app
Copy-Item (Join-Path $sofaBuild 'dsptb-sofa.exe') $app
Copy-Item $notices $app
Invoke-Checked powershell -NoProfile -ExecutionPolicy Bypass -File tools/check-package.ps1 -BuildDir $build -Stage $app
New-Zip (Split-Path $app -Parent) (Join-Path $dist "$appName.zip")

# --- SDK ---
$sdkName = "dsptb-sdk-$Version"
$sdk = Join-Path $stage "sdk/$sdkName"
Install-Component sdk $sdk
Copy-Item $notices $sdk
New-Zip (Split-Path $sdk -Parent) (Join-Path $dist "$sdkName.zip")

# --- Source: the committed tree plus the ASIO SDK files the build used (GPLv3 Corresponding Source) ---
$sourceName = "dsptb-source-$Version"
$src = Join-Path $stage "source/$sourceName"
New-Item -ItemType Directory -Force $src | Out-Null
$tar = Join-Path $stage 'source.tar'
Invoke-Checked git archive --format=tar -o $tar HEAD
Push-Location $src
try { Invoke-Checked cmake -E tar xf $tar } finally { Pop-Location }
$asioOut = Join-Path $src 'third_party/asio-sdk/ASIOSDK'
New-Item -ItemType Directory -Force (Join-Path $asioOut 'common') | Out-Null
Copy-Item (Join-Path $asioSdk 'LICENSE.txt') $asioOut
foreach ($f in 'iasiodrv.h', 'asio.h', 'asiosys.h', 'LICENSE.txt') {
    Copy-Item (Join-Path $asioSdk "common/$f") (Join-Path $asioOut 'common')
}
New-Zip (Split-Path $src -Parent) (Join-Path $dist "$sourceName.zip")

# --- Checksums of exactly this version's zips ---
$sums = Get-ChildItem $dist -Filter "*$Version*.zip" | Sort-Object Name | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $dist "SHA256SUMS-$Version.txt"), (($sums -join "`n") + "`n"), $utf8)
Write-Host "Done: $dist"

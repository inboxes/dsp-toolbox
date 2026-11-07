<#
.SYNOPSIS
  Checks what an app package contains: the bundled modules, and nothing separate, private or test-only.
.PARAMETER Stage
  An unzipped app package to check. Without it, the app component of -BuildDir is installed to a temp folder.
#>
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [string]$Stage = ''
)
$ErrorActionPreference = 'Stop'

$temp = ''
if (-not $Stage) {
    $temp = Join-Path ([IO.Path]::GetTempPath()) ("dsptb-pkg-" + [guid]::NewGuid().ToString('N'))
    & cmake --install $BuildDir --component app --prefix $temp | Out-Null
    if ($LASTEXITCODE) { throw 'cmake --install failed' }
    $Stage = $temp
}

try {
    $errors = @()
    $modulesDir = Join-Path $Stage 'modules'
    $found = @(Get-ChildItem $modulesDir -Directory | ForEach-Object Name)

    foreach ($id in 'org.dsptb.gain', 'org.dsptb.levelmeter', 'org.dsptb.spectrogram') {
        if ($found -notcontains $id) { $errors += "Missing module: $id" }
    }
    foreach ($id in $found) {
        if ($id -notlike 'org.dsptb.*') { $errors += "Unexpected module in package: $id" }
    }

    $separate = @(Get-Content (Join-Path $BuildDir 'packaging/separate-modules.txt') | Where-Object { $_ })
    foreach ($id in $separate) {
        if ($found -contains $id) { $errors += "Separately released module in the app package: $id" }
    }

    $listFile = Join-Path $BuildDir 'packaging/unpackaged-modules.txt'
    $unpackaged = @(Get-Content $listFile | Where-Object { $_ })
    if ($unpackaged.Count -eq 0) { $errors += "$listFile is empty, so the private-module check would prove nothing" }
    foreach ($id in $unpackaged) {
        if ($found -contains $id) { $errors += "Private or test module in package: $id" }
    }

    # Manifests catch a private module copied under another folder name.
    foreach ($m in Get-ChildItem $modulesDir -Recurse -Filter module.json) {
        $id = (Get-Content $m.FullName -Raw | ConvertFrom-Json).id
        if ($unpackaged -contains $id) { $errors += "Private or test manifest in package: $($m.FullName)" }
    }

    foreach ($f in 'LICENSE', 'MODULE-EXCEPTION.md', 'README.md', 'dsptb-cli.exe') {
        if (-not (Test-Path (Join-Path $Stage $f))) { $errors += "Missing file: $f" }
    }
    foreach ($bad in Get-ChildItem $Stage -Recurse -File -Include '*.pdb', '*.lib', '*.exp', '*.ilk', 'asio.h', 'iasiodrv.h') {
        $errors += "File that should not ship: $($bad.FullName)"
    }

    if ($errors) {
        $errors | ForEach-Object { Write-Host "FAIL: $_" }
        exit 1
    }
    Write-Host "Package OK: $($found.Count) modules; $($unpackaged.Count) private or test modules absent"
} finally {
    if ($temp) { Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue }
}

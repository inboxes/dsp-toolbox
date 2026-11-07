<#
.SYNOPSIS
  Proves a module can be made from the packaged SDK alone: installs the SDK to a temp folder, scaffolds a
  processor and an analyzer there, builds them out of tree, and validates them. Needs the MSVC environment.
#>
param([Parameter(Mandatory)][string]$BuildDir)
$ErrorActionPreference = 'Stop'

$temp = Join-Path ([IO.Path]::GetTempPath()) ("dsptb-sdk-" + [guid]::NewGuid().ToString('N'))
# No named parameters: PowerShell would bind an argument such as cmake's -E to a parameter called -exe.
function Invoke-Checked {
    $exe, $rest = $args
    & $exe @rest
    if ($LASTEXITCODE) { throw "$($args -join ' ') failed with exit code $LASTEXITCODE" }
}
try {
    $sdk = Join-Path $temp 'sdk'
    Invoke-Checked cmake --install $BuildDir --component sdk --prefix $sdk
    foreach ($kind in 'processor', 'analyzer') {
        $id = "example.sdk-$kind"
        $folder = Join-Path $temp 'mods'
        & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $sdk 'tools/new-module.ps1') `
            -Name "SDK $kind" -Id $id -Kind $kind -OutDir $folder
        if ($LASTEXITCODE) { throw 'new-module.ps1 failed' }
        $source = Join-Path $folder "sdk_$kind"
        $binary = Join-Path $source 'build'
        # No toolchain file: the SDK must be enough on its own.
        Invoke-Checked cmake -S $source -B $binary -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl `
            -DCMAKE_CXX_COMPILER=cl "-DCMAKE_PREFIX_PATH=$sdk"
        Invoke-Checked cmake --build $binary
        Invoke-Checked (Join-Path $sdk 'bin/dsptb-cli.exe') validate (Join-Path $binary "bin/modules/$id")
    }
    Write-Host 'SDK OK: processor and analyzer templates built out of tree and validated'
} finally {
    Remove-Item $temp -Recurse -Force -ErrorAction SilentlyContinue
}

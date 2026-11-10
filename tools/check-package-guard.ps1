<#
.SYNOPSIS
  Self-test of check-no-gpu.ps1: a clean folder passes, and each kind of GPU content makes it fail.
#>
$ErrorActionPreference = 'Stop'
$check = Join-Path $PSScriptRoot 'check-no-gpu.ps1'
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("dsptb-guard-" + [guid]::NewGuid().ToString('N'))
function Run($dir) { & powershell -NoProfile -ExecutionPolicy Bypass -File $check -Folder $dir | Out-Null; $LASTEXITCODE }
try {
    $ok = Join-Path $tmp 'clean/modules/org.dsptb.gain'
    New-Item -ItemType Directory -Force $ok | Out-Null
    '{"id":"org.dsptb.gain"}' | Set-Content (Join-Path $ok 'module.json')
    if ((Run (Split-Path (Split-Path $ok))) -ne 0) { throw 'A clean folder must pass' }
    $cases = @{
        'runtime folder' = { param($d) New-Item -ItemType Directory -Force "$d/gpu-runtime/bin" | Out-Null; 'x' | Set-Content "$d/gpu-runtime/bin/a.txt" }
        'torch dll'      = { param($d) 'x' | Set-Content "$d/torch_cuda.dll" }
        'ort dll'        = { param($d) 'x' | Set-Content "$d/onnxruntime.dll" }
        'gpu module'     = { param($d) New-Item -ItemType Directory -Force "$d/m" | Out-Null; '{"id":"x","requires":{"gpuRuntime":1}}' | Set-Content "$d/m/module.json" }
    }
    foreach ($name in $cases.Keys) {
        $d = Join-Path $tmp "bad-$($name -replace ' ','-')"
        New-Item -ItemType Directory -Force $d | Out-Null
        & $cases[$name] $d
        if ((Run $d) -eq 0) { throw "Guard missed: $name" }
    }
    Write-Host 'package_guard: OK (clean passes; runtime folder, torch dll, ort dll and GPU module are refused)'
} finally { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }

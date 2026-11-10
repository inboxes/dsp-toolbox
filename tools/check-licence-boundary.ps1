<#
.SYNOPSIS
  Fails if host code (app, core, cli, sdk) includes a CUDA, ONNX Runtime or LibTorch header.
.DESCRIPTION
  The GPL host reaches the MIT GPU runtime only through dsptb/gpu_runtime.h (docs/GPU_RUNTIME.md section 8.4).
  The link side is checked in the top-level CMakeLists.txt.
#>
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
$forbidden = '^\s*#\s*include\s*[<"](cuda[\w./]*|cudnn[\w./]*|cublas[\w./]*|nvml[\w./]*|nvrtc[\w./]*|onnxruntime[\w./]*|torch/[\w./]*|c10/[\w./]*|ATen/[\w./]*|caffe2/[\w./]*)[>"]'
$bad = 0
foreach ($dir in 'app', 'core', 'cli', 'sdk') {
    $path = Join-Path $Root $dir
    if (-not (Test-Path $path)) { continue }
    Get-ChildItem $path -Recurse -Include *.h, *.hpp, *.c, *.cc, *.cpp, *.cxx, *.inl | ForEach-Object {
        $file = $_
        Select-String -Path $file.FullName -Pattern $forbidden | ForEach-Object {
            Write-Host "FAIL: $($file.FullName):$($_.LineNumber): $($_.Line.Trim())"
            $script:bad++
        }
    }
}
if ($bad) { Write-Host "licence_boundary: $bad forbidden include(s) in host code"; exit 1 }
Write-Host 'licence_boundary: OK (no CUDA, ONNX Runtime or LibTorch include in app/ core/ cli/ sdk/)'

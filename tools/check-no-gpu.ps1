<#
.SYNOPSIS
  Fails if a folder that is about to be zipped holds anything of the GPU runtime, or a module that needs it.
.DESCRIPTION
  The GPU runtime is dev-only (docs/GPU_RUNTIME.md section 8.3). make-release.ps1 calls this on every staged
  folder before zipping it. The package_guard ctest runs it on a clean and a contaminated folder.
#>
param([Parameter(Mandatory)][string]$Folder)
$ErrorActionPreference = 'Stop'
$errors = @()
foreach ($d in Get-ChildItem $Folder -Recurse -Directory -Filter 'gpu-runtime') { $errors += "GPU runtime folder: $($d.FullName)" }
$names = 'dsptb_gpu.dll', 'dsptb_gpu.lib', 'gpu-runtime.json', 'onnxruntime*.dll', 'torch*.dll', 'c10*.dll', 'cudart64*.dll',
         'cublas*.dll', 'cudnn*.dll', 'nvrtc*.dll', 'nvJitLink*.dll', 'cufft*.dll', 'curand*.dll'
foreach ($f in Get-ChildItem $Folder -Recurse -File -Include $names) { $errors += "GPU runtime file: $($f.FullName)" }
foreach ($m in Get-ChildItem $Folder -Recurse -File -Filter module.json) {
    $json = Get-Content $m.FullName -Raw | ConvertFrom-Json
    if ($json.PSObject.Properties.Name -contains 'requires' -and $json.requires.PSObject.Properties.Name -contains 'gpuRuntime') {
        $errors += "Module that needs the GPU runtime: $($m.FullName)"
    }
}
if ($errors) {
    $errors | ForEach-Object { Write-Host "FAIL: $_" }
    Write-Host 'The GPU runtime and GPU modules are dev-only and must not be packaged.'
    exit 1
}
Write-Host "No GPU runtime content in $Folder"

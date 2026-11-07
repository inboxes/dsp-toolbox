<#
.SYNOPSIS
  Creates a new module folder from the template (MODULE_SDK section 6).
.EXAMPLE
  tools/new-module.ps1 -Name "My Filter" -Id me.myfilter -Kind processor -OutDir C:/dev/modules
.PARAMETER License
  SPDX id written to module.json. GPL-3.0-only is the default; see MODULE-EXCEPTION.md for other licences.
#>
param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9]+(\.[a-z0-9-]+)+$')][string]$Id,
    [ValidateSet('processor', 'analyzer')][string]$Kind = 'processor',
    [string]$OutDir = '.',
    [string]$License = 'GPL-3.0-only'
)
$ErrorActionPreference = 'Stop'

# The script sits in tools/, in the repo and in the SDK zip alike.
$template = Join-Path (Split-Path $PSScriptRoot -Parent) 'module-template'
if (-not (Test-Path $template)) { $template = Join-Path $PSScriptRoot 'module-template' }
if (-not (Test-Path $template)) { throw "Module template not found near $PSScriptRoot" }

if ($Name -match '["\\]') { throw 'The name may not contain quotes or backslashes.' }

# The target and DLL name come from the last part of the id.
$target = ($Id -split '\.')[-1] -replace '-', '_'
if ($target -notmatch '^[a-z_]') { $target = "m_$target" }
$class = (Get-Culture).TextInfo.ToTitleCase(($target -replace '_', ' ')) -replace ' ', ''

$dest = Join-Path $OutDir $target
if (Test-Path $dest) { throw "$dest already exists." }
New-Item -ItemType Directory -Path (Join-Path $dest 'src') | Out-Null

$source = "$Kind.cpp.in"
$files = @{
    'CMakeLists.txt.in' = 'CMakeLists.txt'
    'module.json.in'    = 'module.json'
    'README.md.in'      = 'README.md'
    $source             = "src/$target.cpp"
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($from in $files.Keys) {
    $text = [IO.File]::ReadAllText((Join-Path $template $from))
    $text = $text.Replace('@MODULE_ID@', $Id).Replace('@MODULE_NAME@', $Name).Replace('@TARGET@', $target)
    $text = $text.Replace('@CLASS@', $class).Replace('@KIND@', $Kind).Replace('@LICENSE@', $License)
    [IO.File]::WriteAllText((Join-Path $dest $files[$from]), $text, $utf8)
}
Write-Host "Created $dest"

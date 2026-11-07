<#
.SYNOPSIS
  Writes one version's section of CHANGELOG.md to a file, for the release notes. A pre-release (0.1.0-rc.1) uses
  the section of its final version.
#>
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$OutFile,
    [string]$File = 'CHANGELOG.md'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$base = $Version -replace '-.*$', ''

$section = @()
$inside = $false
foreach ($line in Get-Content (Join-Path $root $File) -Encoding UTF8) {
    if ($line -match '^## \[') {
        if ($inside) { break }
        $inside = $line -match ('^## \[' + [regex]::Escape($base) + '\]')
        continue
    }
    if ($inside) { $section += $line }
}
$text = ($section -join "`n").Trim()
if (-not $text) { throw "$File has no section for $base." }
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($OutFile, $text + "`n", $utf8)
Write-Host "Wrote the notes for $base to $OutFile"

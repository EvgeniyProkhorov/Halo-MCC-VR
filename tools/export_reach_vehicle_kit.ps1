param(
    [string]$HrekRoot = 'N:\SteamLibrary\steamapps\common\HREK',
    [string]$OutputRoot = '',
    [switch]$Refresh
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputRoot) {
    $OutputRoot = Join-Path $repoRoot 'out\reach-vehicle-kit-source\canonical'
}
$manifestPath = Join-Path $PSScriptRoot 'reach_vehicle_kit_manifest.json'
$tool = Join-Path $HrekRoot 'tool.exe'
$tagsRoot = Join-Path $HrekRoot 'tags'

if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
    throw "HREK tool not found: $tool"
}
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Kit manifest not found: $manifestPath"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$assets = @($manifest.identities) + @($manifest.reference_assets)
$seen = @{}
$uniqueAssets = foreach ($asset in $assets) {
    if (-not $seen.ContainsKey($asset.id)) {
        $seen[$asset.id] = $true
        $asset
    }
}

New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$resolvedOutput = (Resolve-Path -LiteralPath $OutputRoot).Path
$resolvedRepo = (Resolve-Path -LiteralPath $repoRoot).Path
$repoPrefix = $resolvedRepo.TrimEnd([IO.Path]::DirectorySeparatorChar) +
    [IO.Path]::DirectorySeparatorChar
if ($resolvedOutput -ne $resolvedRepo -and
    -not $resolvedOutput.StartsWith(
        $repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Output must stay under this repository: $resolvedOutput"
}

function Invoke-HrekTool {
    param([string[]]$Arguments)
    & $tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "HREK tool failed ($LASTEXITCODE): $($Arguments -join ' ')"
    }
}

function Export-XmlIfNeeded {
    param(
        [string]$TagPath,
        [string]$OutputPath
    )
    if (-not $Refresh -and (Test-Path -LiteralPath $OutputPath) -and
        (Get-Item -LiteralPath $OutputPath).Length -gt 0) {
        return
    }
    if (Test-Path -LiteralPath $OutputPath) {
        Remove-Item -LiteralPath $OutputPath -Force
    }
    Invoke-HrekTool @('export-tag-to-xml', $TagPath, $OutputPath)
    if (-not (Test-Path -LiteralPath $OutputPath) -or
        (Get-Item -LiteralPath $OutputPath).Length -eq 0) {
        throw "HREK produced an empty XML export: $OutputPath"
    }
}

Push-Location $HrekRoot
try {
    foreach ($asset in $uniqueAssets) {
        $id = [string]$asset.id
        $tag = [string]$asset.tag
        Write-Host "Exporting Reach authoring source: $id"

        foreach ($extension in @('vehicle', 'model', 'render_model')) {
            $tagPath = Join-Path $tagsRoot ($tag + '.' + $extension)
            if (-not (Test-Path -LiteralPath $tagPath -PathType Leaf)) {
                throw "Missing official HREK tag: $tagPath"
            }
            $outPath = Join-Path $resolvedOutput ($id + '.' + $extension + '.xml')
            Export-XmlIfNeeded -TagPath $tagPath -OutputPath $outPath
        }

        $meshPath = Join-Path $resolvedOutput ($id + '.mesh.x')
        if ($Refresh -or -not (Test-Path -LiteralPath $meshPath -PathType Leaf) -or
            (Get-Item -LiteralPath $meshPath).Length -eq 0) {
            $meshPrefix = Join-Path $resolvedOutput ($id + '.mesh')
            Invoke-HrekTool @('export-render-model-mesh', $tag, $meshPrefix)
            if (-not (Test-Path -LiteralPath $meshPath -PathType Leaf) -or
                (Get-Item -LiteralPath $meshPath).Length -eq 0) {
                throw "HREK produced no DirectX mesh: $meshPath"
            }
        }
    }
}
finally {
    Pop-Location
}

$meshCount = (Get-ChildItem -LiteralPath $resolvedOutput -Filter '*.mesh.x').Count
$xmlCount = (Get-ChildItem -LiteralPath $resolvedOutput -Filter '*.xml').Count
Write-Host "Reach source ready: $meshCount meshes, $xmlCount XML exports"
Write-Host $resolvedOutput

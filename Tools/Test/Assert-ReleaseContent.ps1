#Requires -Version 7
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$OpenScct,
    [string]$Engine = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { 'F:\UE_5.8' }),
    [string]$Report,
    # The imported levels the release bundles (Content/Imported/Maps/<Map>/<Map>.umap).
    [string[]]$ImportedMaps = @()
)

$ErrorActionPreference = 'Stop'
$project = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$stageRoot = (Resolve-Path -LiteralPath $Stage).Path
$target = 'OpenSCCTUnreal'
$projectPackages = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$tracked = @(git -c core.quotepath=false -C $project ls-files -- Content/OpenSCCT)
if ($LASTEXITCODE -ne 0 -or !$tracked.Count) { throw 'Cannot read project-owned assets from Git.' }
foreach ($path in $tracked) {
    if ($path -match '\.(uasset|umap)$') { [void]$projectPackages.Add(($path -replace '\.(uasset|umap)$', '')) }
}

$plugin = Join-Path $project 'Plugins/glTFRuntime'
$pluginAssets = @(git -C $plugin ls-files -- Content)
if ($LASTEXITCODE -ne 0 -or !$pluginAssets.Count) { throw 'Missing pinned glTFRuntime assets.' }
foreach ($path in $pluginAssets) {
    if ($path -match '\.(uasset|umap)$') { [void]$projectPackages.Add("Plugins/glTFRuntime/" + ($path -replace '\.(uasset|umap)$', '')) }
}
$cooked = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$importedSeen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($manifest in 'Manifest_UFSFiles_Win64.txt', 'Manifest_NonUFSFiles_Win64.txt') {
    $file = Join-Path $stageRoot $manifest
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing Unreal staging inventory $manifest." }
    foreach ($line in [IO.File]::ReadLines($file)) {
        $name = ($line -split "`t", 2)[0].Replace('\', '/')
        if (![string]::IsNullOrWhiteSpace($name)) { [void]$seen.Add($name) }
    }
}
# Non-UFS runtime dependencies can also be laid directly into the archive by packaging tools.
foreach ($file in Get-ChildItem -LiteralPath (Join-Path $stageRoot $target) -Recurse -File) {
    [void]$seen.Add([IO.Path]::GetRelativePath($stageRoot, $file.FullName).Replace('\', '/'))
}
foreach ($path in $seen) {
    if ($path -notlike "$target/*") { continue }
    $relative = $path.Substring($target.Length + 1)
    # The cooked editor imports under Content/Imported ship with the release. Loose converted files never do.
    $cookedImport = $relative -match '(?i)^Content/Imported/(?:Maps|Chars|Fx)/.+\.(?:uasset|umap|uexp|ubulk|uptnl)$'
    if ($cookedImport) { [void]$importedSeen.Add($relative); continue }
    if ($relative -match '(?:^|/)(?:Imported|shared|svmap|\.content|packs)(?:/|$)' -and $relative -notlike 'Binaries/DotNet/*') {
        throw "Retail or converted content staged at $path."
    }
    if ($relative -match '\.(?:sdc|sds|usx|utx|utc|ukx|uax|uas|mux|u|umd|bik|gpr)$') {
        throw "Original game package staged at $path."
    }
    if ($relative -eq 'Content/Splash/Splash.bmp') {
        if ((Get-FileHash -LiteralPath (Join-Path $stageRoot $path)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $project $relative)).Hash) {
            throw 'The staged Windows splash differs from the project artwork.'
        }
        continue
    }
    if ($relative -notlike 'Binaries/Bridge/maps/*' -and $relative -match '\.(glb|gltf|ogg|wav|dds|png|jpg|jpeg|tga|bmp)$') {
        throw "Unexpected loose media in the project staging inventory: $path."
    }
    if (($relative -like 'Content/*' -or $relative -like 'Plugins/*/Content/*') -and $relative -match '(?i)^(.+?)(?:\.m)?\.(uasset|umap|uexp|ubulk|uptnl)$') {
        $packageName = $Matches[1]
        if (!$projectPackages.Contains($packageName)) { throw "Untracked cooked project package at $path." }
        [void]$cooked.Add($packageName)
    }
    if ($relative -like 'Binaries/Bridge/*' -and $relative -notlike 'Binaries/Bridge/maps/*' -and
        $relative -notmatch '^Binaries/Bridge/[^/]+\.(?:dll|pdb|deps\.json|runtimeconfig\.json)$') {
        throw "Unexpected loose bridge dependency at $path."
    }
}
foreach ($required in 'Content/OpenSCCT/Maps/L_FrontEnd', 'Content/OpenSCCT/RuntimeMap/L_RuntimeMap') {
    if (!$seen.Contains("$target/$required.umap")) { throw "Missing required cooked level $required." }
}

$unbundled = @($ImportedMaps | Where-Object { !$importedSeen.Contains("Content/Imported/Maps/$_/$_.umap") })
if ($unbundled.Count) { throw "Imported maps missing from the cook (check DirectoriesToAlwaysCook=/Game/Imported/Maps): $($unbundled -join ', ')." }
foreach ($folder in 'Chars', 'Fx') {
    if (!($importedSeen | Where-Object { $_ -like "Content/Imported/$folder/*" } | Select-Object -First 1)) {
        throw "No cooked Content/Imported/$folder assets in the release."
    }
}
Write-Host "Staged $($importedSeen.Count) cooked files from /Game/Imported ($($ImportedMaps.Count) imported levels)."

if (!$seen.Contains("$target/Plugins/glTFRuntime/Content/M_glTFRuntimeBase.uasset")) {
    throw 'Missing cooked glTFRuntime material for local skeletal meshes.'
}

# A mesh can be present in the container with missing material slots after the cook strips editor imports.
# Inspect the resolved runtime objects and their textures directly in the shipped IoStore packages.
$toc = Join-Path $stageRoot "$target/Content/Paks/global.utoc"
$description = Join-Path ([IO.Path]::GetTempPath()) ("scct-weapon-imports-" + [Guid]::NewGuid() + ".txt")
try {
    & (Join-Path $Engine 'Engine/Binaries/Win64/UnrealPak.exe') IoStore "-Describe=$toc" '-PackageFilter=/Game/OpenSCCT/Weapons/SM_Merc*' "-DumpToFile=$description" | Out-Null
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $description -PathType Leaf)) { throw 'Could not inspect cooked weapon attachments.' }
    & python (Join-Path $PSScriptRoot 'verify_attachment_imports.py') $description
    if ($LASTEXITCODE -ne 0) { throw 'The cooked weapon attachments have incomplete material dependencies.' }
} finally {
    if (Test-Path -LiteralPath $description) { Remove-Item -LiteralPath $description -Force }
}

$fixtures = @{}
foreach ($fixture in 'hub', 'hubmap', 'testbox') {
    $fixtureFiles = @(git -c core.quotepath=false -C $OpenScct ls-files -- "fixtures/$fixture/")
    if ($LASTEXITCODE -ne 0 -or !$fixtureFiles.Count) { throw "Missing source fixture $fixture." }
    foreach ($file in $fixtureFiles) { $fixtures[$file.Substring('fixtures/'.Length)] = Join-Path $OpenScct $file }
}
$loose = @(Get-ChildItem -LiteralPath (Join-Path $stageRoot "$target/Binaries/Bridge/maps") -File -Recurse)
$found = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($file in $loose) {
    $key = [IO.Path]::GetRelativePath((Join-Path $stageRoot "$target/Binaries/Bridge/maps"), $file.FullName).Replace('\', '/')
    if (!$fixtures.ContainsKey($key) -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $fixtures[$key]).Hash) {
        throw "Unexpected or modified loose map file $key."
    }
    [void]$found.Add($key)
}
$missing = @($fixtures.Keys | Where-Object { !$found.Contains($_) })
if ($missing.Count) { throw "Missing project map fixture files: $($missing -join ', ')." }
$result = [ordered]@{ cookedProjectPackages = @($cooked | Sort-Object); projectFixtureFiles = $found.Count }
if ($Report) { $result | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $Report -Encoding utf8NoBOM }
Write-Host "Verified $($cooked.Count) cooked project packages and $($found.Count) loose project fixture files."

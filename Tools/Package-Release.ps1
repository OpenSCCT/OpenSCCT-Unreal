#Requires -Version 7
<#
.SYNOPSIS
Packages an OpenSCCT Unreal release: the zip the OpenSCCT launcher installs, its checksums, the launcher handshake,
draft notes and (with -Repository) the command that uploads them as a draft GitHub release.

.DESCRIPTION
Builds the C# bridge against the simulation in Runtime/, lays a private .NET runtime next to it, then has Unreal build,
cook, stage and archive the game (RunUAT BuildCookRun) and writes the release folder:

  OpenSCCT Unreal\
    OpenSCCTUnreal.exe                 the starter: double-click to run
    OpenSCCTUnreal\Binaries\Win64\     the game (OpenSCCTUnreal-Win64-Shipping.exe)
    OpenSCCTUnreal\Binaries\Bridge\    the bridge and OpenSCCT's assemblies
    OpenSCCTUnreal\Binaries\DotNet\    the private .NET runtime (players need none installed)
    OpenSCCTUnreal\Content\Paks\       the cooked project frontend, runtime scene, materials and models
    Engine\                            the engine's staged files
    Prerequisites\vc_redist.x64.exe    the Visual C++ runtime installer
    README.txt                         what it is, the notices
    THIRD-PARTY-NOTICES.md             the third-party components the release ships and their licences
    version.json                       version, commits, the names the release owns and every file's SHA-256

then zips it (one top folder) and writes, in -Out:

  OpenSCCT-Unreal-<version>-win-x64.zip  and  .zip.sha256     the download and its checksum
  OpenSCCT-Unreal-<version>-win-x64.launcher.json             the launcher handshake
  SHA256SUMS                                                  the checksum line, for people and scripts
  notes.md                                                    release notes drafted from the commits since the last tag
  publish.ps1                                                 uploads it all (only with -Repository)
  unreal-files\                                               what the launcher updates from (-StoreTag release):
    OpenSCCT-Unreal-<version>-win-x64-files.json (+ .sha256)  every file with its SHA-256, its blob and its patches
    <sha256>.gz, OpenSCCT-Unreal-<version>-win-x64-pack-<hash>.zip   new file contents (the launcher's game delta format)
    <old sha256>-<new sha256>.osdiff                          binary patches from the -Base releases' files

The file list is the launcher's game format (BuildFiles in OpenSCCT's launcher, written by its New-Release.ps1): a file
the player already has is kept, a new one is downloaded gzipped. A file of 1 MB or more that changed since a base
release also gets a patch from that release's version of it (Tools/ReleaseDelta, bsdiff's method): the exe patches to
a few MB instead of 66, the cooked content to under 1 MB for a handful of changed assets instead of 494. Launchers that
do not know patches ignore them and take the blob.

The setup tool included with the game builds retail maps and shared assets from the player's Steam installation. The release cook uses
project-owned sources. Its manifests and built-in loose fixtures are checked before packaging.

.PARAMETER Version
The version to package. Default: ProjectVersion in Config/DefaultGame.ini, which it must match. A pre-release adds a
suffix: 0.2.0-beta.1.

.PARAMETER Local
A package for trying the game out on this machine: skips the release preconditions (clean trees, pushed to
origin/main, tag not taken), allows any -Version, warns instead of failing on private paths and size, and writes no
publish.ps1. Never upload a local package.

.PARAMETER Configuration
Shipping (default) or Development. A Development package keeps the console and logging; only -Local may make one.

.PARAMETER Out
The output folder. Default Saved/Release/<version>.

.PARAMETER Engine
The Unreal Engine 5.8 root (the folder holding Engine\). Default: UE_ROOT, else F:\UE_5.8.

.PARAMETER Repository
The GitHub repository (owner/name) the release is published to. Players' launchers read its releases without a
token, so it must be public. Default OpenSCCT/OpenSCCT-Unreal. Pass -Repository '' to write no publish.ps1.

.PARAMETER CheckContentOnly
Run only the provenance check of the source assets and stop.

.PARAMETER NoDotNet
Omit the private .NET runtime and standalone setup from a developer-only local package.

.PARAMETER Base
Earlier releases to make patches from and whose blobs this release reuses: their output folders (holding the
OpenSCCT Unreal folder and unreal-files\). Default: the two newest older versions under Saved/Release.

.PARAMETER StoreTag
The release that holds the blobs and patches, shared by every version. Default unreal-files-2.

.PARAMETER BuildDrive
The drive letter the game is built from (see the build step). Always the same letter, so the same code builds the same
exe. Default U.

.EXAMPLE
pwsh Tools/Package-Release.ps1
pwsh Tools/Package-Release.ps1 -Local -CheckContentOnly
pwsh Tools/Package-Release.ps1 -Local -Version 0.1.1 -Configuration Development
#>
param(
    [string]$Version = '',
    [switch]$Local,
    [ValidateSet('Shipping', 'Development')]
    [string]$Configuration = 'Shipping',
    [string]$Out = '',
    [string]$Engine = $(if ($env:UE_ROOT) { $env:UE_ROOT } else { 'F:\UE_5.8' }),
    [string]$Repository = 'OpenSCCT/OpenSCCT-Unreal',
    [switch]$NoDotNet,
    [string[]]$Base = @(),
    [string]$StoreTag = 'unreal-files-2',
    [ValidatePattern('^[A-Za-z]$')]
    [string]$BuildDrive = 'U',
    [switch]$CheckContentOnly
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$project = Join-Path $repo 'OpenSCCTUnreal.uproject'
$target = 'OpenSCCTUnreal'
$platform = 'win-x64'
$folderName = 'OpenSCCT Unreal'
$product = 'OpenSCCT Unreal'
$starter = "$target.exe"
$gameExe = if ($Configuration -eq 'Shipping') { "$target-Win64-Shipping.exe" } else { "$target.exe" }
# What a GitHub release asset may weigh, with room to spare.
$assetLimit = 1.9GB

function Fail([string]$message) { throw "Package-Release: $message" }
function Invoke-Native([string]$what, [scriptblock]$command) {
    & $command
    if ($LASTEXITCODE -ne 0) { Fail "$what failed (exit code $LASTEXITCODE)" }
}
function Warn-Or-Fail([string]$message) { if ($Local) { Write-Warning $message } else { Fail $message } }

# --- Provenance of the source assets --------------------------------------------------------------------------------
# An asset imported from a file keeps that file's path in its AssetImportData, which the editor saves into the .uasset
# (as Latin-1 or UTF-16 text). Every asset under Content/ outside Imported/ may be cooked (/Game/OpenSCCT
# always is), so none of them may come from the converted original game: no import path into a .content folder, an
# svmap folder or the shared chars or maps folders, and no SPersoTextures_ file. A Git LFS pointer cannot be checked.
# Returns one line per problem.
function Test-ContentProvenance([string]$contentRoot) {
    $skip = @('Imported') | ForEach-Object { [IO.Path]::GetFullPath((Join-Path $contentRoot $_)).TrimEnd('\') + '\' }
    $marker = [regex]::new('(?i)[\\/](?:\.content|svmap|shared[\\/](?:chars|maps))[\\/]|(?<![A-Za-z0-9])SPersoTextures_', 'Compiled')
    # The characters a path may hold, to widen a match to the whole path for the report.
    $pathChar = [regex]::new('^[\x20-\x7E-["<>|*?]]$')
    $lfs = [Text.Encoding]::ASCII.GetBytes('version https://git-lfs.github.com/spec/')
    $latin1 = [Text.Encoding]::Latin1
    $utf16 = [Text.Encoding]::Unicode
    $files = Get-ChildItem -LiteralPath $contentRoot -Recurse -File -Include '*.uasset', '*.umap' |
        Where-Object { $full = $_.FullName; -not ($skip | Where-Object { $full.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }) }
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($contentRoot.TrimEnd('\', '/').Length + 1).Replace('\', '/')
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        if ($bytes.Length -ge $lfs.Length -and [Linq.Enumerable]::SequenceEqual([byte[]]$bytes[0..($lfs.Length - 1)], $lfs)) {
            "Content/${relative}: a Git LFS pointer, not the asset (run git lfs pull)"
            continue
        }
        # UTF-16 text starts at an even or an odd offset of the file: both readings.
        $texts = @($latin1.GetString($bytes), $utf16.GetString($bytes))
        if ($bytes.Length -gt 1) { $texts += $utf16.GetString($bytes, 1, $bytes.Length - 1) }
        $found = [Collections.Generic.SortedSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($text in $texts) {
            foreach ($m in $marker.Matches($text)) {
                $start = $m.Index; $end = $m.Index + $m.Length
                while ($start -gt 0 -and $pathChar.IsMatch([string]$text[$start - 1])) { $start-- }
                while ($end -lt $text.Length -and $pathChar.IsMatch([string]$text[$end])) { $end++ }
                [void]$found.Add($text.Substring($start, $end - $start).Trim())
            }
        }
        foreach ($path in $found) { "Content/${relative}: imported from $path" }
    }
}

$plugin = Join-Path $repo 'Plugins/glTFRuntime'
$pin = [string](git -C $repo ls-files --stage -- Plugins/glTFRuntime)
if ($pin -notmatch '^160000 ([0-9a-f]{40}) ' -or !(Test-Path -LiteralPath (Join-Path $plugin 'glTFRuntime.uplugin'))) {
    Fail 'Initialize the glTFRuntime dependency with git submodule update --init --recursive.'
}
$pinnedCommit = $Matches[1]
if ((git -C $plugin rev-parse HEAD) -ne $pinnedCommit -or (git -C $plugin diff --name-only HEAD -- Content Source)) {
    Fail "Initialize the glTFRuntime dependency at $pinnedCommit before packaging."
}

$provenanceWatch = [Diagnostics.Stopwatch]::StartNew()
$provenance = @(Test-ContentProvenance (Join-Path $repo 'Content'))
$provenanceWatch.Stop()
if ($provenance.Count -gt 0) {
    Warn-Or-Fail ("source assets that would be cooked come from the original game's converted content (move them out of " +
        "Content/ or replace them with project-made ones; Git LFS pointers need git lfs pull):`n  " + ($provenance -join "`n  "))
} else {
    Write-Host ('No source asset comes from converted original content ({0:0.0} s).' -f $provenanceWatch.Elapsed.TotalSeconds)
}
if ($CheckContentOnly) { return }

# --- Bundled maps --------------------------------------------------------------------------------------------------
# The enhanced maps: every level the release machine imported into Content/Imported/Maps/<Map>/<Map>.umap is cooked
# (DirectoriesToAlwaysCook in Config/DefaultGame.ini) and shipped, with the characters, weapons and effects imported
# under Content/Imported/Chars and Content/Imported/Fx. Content/Imported is git-ignored local import output.
$importedRoot = Join-Path $repo 'Content/Imported'
$importedMaps = Join-Path $importedRoot 'Maps'
$bundledMaps = @(Get-ChildItem -LiteralPath $importedMaps -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne 'Hub' -and (Test-Path -LiteralPath (Join-Path $_.FullName "$($_.Name).umap")) } | ForEach-Object Name | Sort-Object)
if ($bundledMaps.Count -eq 0) {
    Fail "no imported maps in $importedMaps (no <Map>\<Map>.umap): run Tools/Import/Import-Map.ps1 on this machine first"
}
foreach ($folder in 'Chars', 'Fx') {
    if (-not (Get-ChildItem -LiteralPath (Join-Path $importedRoot $folder) -Recurse -File -Filter '*.uasset' -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        Fail "Content/Imported/$folder is empty: run Tools/Import/Import-Characters.ps1 and Tools/Effects/Build-EffectAssets.ps1 first"
    }
}
$mapsToCook = @($bundledMaps)
if (Test-Path -LiteralPath (Join-Path $importedMaps 'Hub/Hub.umap')) { $mapsToCook += 'Hub' }
Write-Host "Bundling $($bundledMaps.Count) imported maps: $($bundledMaps -join ', ')"

# --- Version -------------------------------------------------------------------------------------------------------
$gameIni = Get-Content -LiteralPath (Join-Path $repo 'Config/DefaultGame.ini') -Raw
$projectVersion = [regex]::Match($gameIni, '(?m)^ProjectVersion=(\S+)\s*$').Groups[1].Value
if (-not $projectVersion) { Fail 'no ProjectVersion in Config/DefaultGame.ini' }
if (-not $Version) { $Version = $projectVersion }
if ($Version -notmatch '^(\d+\.\d+\.\d+)(?:-([0-9A-Za-z.-]+))?$') { Fail "'$Version' is not a version like 0.2.0 or 0.3.0-beta.1" }
$core = $Matches[1]
$isPrerelease = [bool]$Matches[2]
if (-not $Local -and $core -ne $projectVersion) { Fail "-Version $Version does not match ProjectVersion $projectVersion in Config/DefaultGame.ini (bump it first)" }
if (-not $Local -and $Configuration -ne 'Shipping') { Fail 'a release is a Shipping build; -Configuration Development needs -Local' }

if (-not $Out) { $Out = Join-Path $repo "Saved/Release/$Version" }
$Out = [IO.Path]::GetFullPath($Out)
$folder = Join-Path $Out $folderName
$archive = Join-Path $Out '.archive'
$zipName = "OpenSCCT-Unreal-$Version-$platform.zip"
$zip = Join-Path $Out $zipName
$launcherName = "OpenSCCT-Unreal-$Version-$platform.launcher.json"
$storeDir = Join-Path $Out 'unreal-files'
$listName = "OpenSCCT-Unreal-$Version-$platform-files.json"
$packName = "OpenSCCT-Unreal-$Version-$platform-pack.zip"

# --- Preconditions -------------------------------------------------------------------------------------------------
$commit = git -C $repo rev-parse HEAD 2>$null
if ($LASTEXITCODE -ne 0 -or -not $commit) { Fail 'run it from a git clone of OpenSCCT-Unreal' }
$commit = ([string]$commit).Trim()
if (-not $Local) {
    if (git -C $repo status --porcelain) { Fail 'the working tree has changes; commit or stash them' }
    Invoke-Native 'git fetch' { git -C $repo fetch origin main --quiet --tags }
    git -C $repo merge-base --is-ancestor HEAD origin/main
    if ($LASTEXITCODE -ne 0) { Fail 'HEAD is not on origin/main; push it first' }
    if (git -C $repo ls-remote --tags origin "refs/tags/v$Version") { Fail "the tag v$Version already exists on origin" }
}

$runtimeRoot = Join-Path $repo 'Runtime'
$runtimeTree = ([string](git -C $repo rev-parse 'HEAD:Runtime')).Trim()
if ($LASTEXITCODE -ne 0 -or $runtimeTree -notmatch '^[0-9a-f]{40,64}$') { Fail 'the release must contain the committed Runtime source tree' }
$runtimeProps = [xml](Get-Content -LiteralPath (Join-Path $runtimeRoot 'Directory.Build.props') -Raw)
$runtimeVersion = [string]($runtimeProps.Project.PropertyGroup.VersionPrefix | Where-Object { $_ } | Select-Object -First 1)
if (-not $runtimeVersion) { Fail 'the local Runtime source has no simulation version' }
if ($NoDotNet -and -not $Local) { Fail 'public builds must include the private runtime for the game and its Steam setup tool' }

$uat = Join-Path $Engine 'Engine/Build/BatchFiles/RunUAT.bat'
if (-not (Test-Path -LiteralPath $uat)) { Fail "no Unreal Engine at $Engine (pass -Engine or set UE_ROOT)" }
$engineBuild = Get-Content -LiteralPath (Join-Path $Engine 'Engine/Build/Build.version') -Raw | ConvertFrom-Json
$engineVersion = "$($engineBuild.MajorVersion).$($engineBuild.MinorVersion).$($engineBuild.PatchVersion)"
if ("$($engineBuild.MajorVersion).$($engineBuild.MinorVersion)" -ne '5.8') { Fail "the engine at $Engine is $engineVersion; the project is 5.8" }
$vcRedist = Join-Path $Engine 'Engine/Extras/Redist/en-us/vc_redist.x64.exe'
if (-not (Test-Path -LiteralPath $vcRedist)) { Fail "no $vcRedist in the engine" }

Write-Host "Packaging $product $Version ($Configuration, $commit; simulation $runtimeVersion) into $Out$(if ($Local) { ' (local package)' })"
$localMark = Join-Path $Out '.local'
$publishedMark = Join-Path $Out '.published'
foreach ($stale in @($folder, $archive, $storeDir, $zip, "$zip.sha256", (Join-Path $Out 'SHA256SUMS'), (Join-Path $Out $launcherName),
        (Join-Path $Out 'publish.ps1'), (Join-Path $Out 'notes.md'), $localMark, $publishedMark)) {
    if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Recurse -Force }
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
# A local package is marked, so no real release ever takes it as a base: its blobs were never uploaded.
if ($Local) { Set-Content -LiteralPath $localMark -Value 'A local package: never a base for a published release.' }

# Orders versions as semver does (0.2.0-beta.1 < 0.2.0-beta.10 < 0.2.0), as one string that sorts ordinally.
function Get-SemKey([string]$v) {
    if ($v -notmatch '^(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$') { return $null }
    $key = '{0:D10}.{1:D10}.{2:D10}' -f [long]$Matches[1], [long]$Matches[2], [long]$Matches[3]
    if (-not $Matches[4]) { return "$key~" }
    $pre = ($Matches[4].Split('.') | ForEach-Object { if ($_ -match '^\d+$') { '0{0:D10}' -f [long]$_ } else { "1$_" } }) -join '.'
    return "$key-$pre"
}

# The releases this one updates from: their files (for patches) and their file lists (for the blobs they published).
function Read-BaseRelease([string]$dir) {
    $dir = [IO.Path]::GetFullPath($dir)
    $info = Join-Path $dir "$folderName/version.json"
    if (-not (Test-Path -LiteralPath $info)) { return $null }
    $v = (Get-Content -LiteralPath $info -Raw | ConvertFrom-Json)
    $list = Join-Path $dir "unreal-files/OpenSCCT-Unreal-$($v.version)-$platform-files.json"
    [pscustomobject]@{ Dir = $dir; Folder = (Join-Path $dir $folderName); Version = $v.version; Files = $v.files
        Key = (Get-SemKey $v.version); Published = (Test-Path -LiteralPath (Join-Path $dir '.published'))
        Local = (Test-Path -LiteralPath (Join-Path $dir '.local'))
        SteamAssets = $v.contentSource -eq 'local-steam'
        List = $(if (Test-Path -LiteralPath $list) { (Get-Content -LiteralPath $list -Raw | ConvertFrom-Json).files } else { @() }) }
}
$ownKey = Get-SemKey $Version
$bases = @()
if ($Base.Count -gt 0) {
    foreach ($b in $Base) {
        $read = Read-BaseRelease $b
        if (-not $read) { Fail "-Base $b is not a release output folder (no $folderName\version.json)" }
        if ($read.Folder -eq $folder) { Fail "-Base $b is this release's own output folder" }
        if (-not $Local -and -not $read.Published) { Fail "-Base $b was never published (no .published): its blobs are not on GitHub" }
        $bases += $read
    }
} else {
    # The two newest older versions, one folder each (a published one first). A release only takes published ones; a
    # local package may take any, to try updates out.
    $older = @(Get-ChildItem -LiteralPath (Join-Path $repo 'Saved/Release') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -ne $Out } | ForEach-Object { Read-BaseRelease $_.FullName } |
        Where-Object { $_ -and $_.Key -and [string]::CompareOrdinal($_.Key, $ownKey) -lt 0 -and ($Local -or $_.Published) } |
        Group-Object Key | ForEach-Object { $_.Group | Sort-Object @{ e = { $_.Published }; Descending = $true }, @{ e = { $_.Local } }, Dir | Select-Object -First 1 })
    # Newest first by the semver key, compared ordinally (Sort-Object compares strings by culture).
    $sorted = [System.Collections.Generic.List[object]]::new()
    foreach ($o in $older) { $sorted.Add($o) }
    $sorted.Sort([Comparison[object]] { param($x, $y) [string]::CompareOrdinal($y.Key, $x.Key) })
    $bases = @($sorted | Select-Object -First 2)
}
if ($bases.Count -gt 0) { Write-Host "Updates from: $(($bases | ForEach-Object { "$($_.Version) ($(Split-Path -Leaf $_.Dir))" }) -join ', ')" }
else { Write-Host 'No earlier release to update from: no patches.' }

Write-Host 'Building the patch tool...'
$deltaTool = Join-Path $repo 'Saved/Tools/ReleaseDelta/ReleaseDelta.exe'
Invoke-Native 'dotnet build (ReleaseDelta)' {
    dotnet build (Join-Path $repo 'Tools/ReleaseDelta') -c Release -o (Split-Path -Parent $deltaTool) -nologo -v:q -clp:ErrorsOnly
}
Invoke-Native 'ReleaseDelta selftest' { & $deltaTool selftest | Out-Null }

# --- Bridge and .NET runtime ---------------------------------------------------------------------------------------
# Keep the release runtime outside the project. Editors may be using it there, and an incremental
# UBT build can reuse a target receipt written before Binaries/DotNet existed.
$dotNetFolder = Join-Path $Out '.private-dotnet'
$madeDotNet = $false
try {
    Write-Host 'Building the bridge...'
    Invoke-Native 'dotnet build (bridge)' {
        dotnet build (Join-Path $repo 'Bridge/OpenScct.UnrealBridge') -c Release `
            -p:ContinuousIntegrationBuild=true -nologo -v:q -clp:ErrorsOnly
    }

    if (-not $NoDotNet) {
        if (Test-Path -LiteralPath $dotNetFolder) { Fail "private runtime staging directory already exists: $dotNetFolder" }
        # The installed runtime, laid out as an install root: host\fxr\<v>\hostfxr.dll and shared\Microsoft.NETCore.App\<v>
        # (what FScctDotNetHost::FindHostFxr looks for). The newest 10.x, as the bridge's runtimeconfig rolls forward.
        $dotnetRoot = Split-Path -Parent (Get-Command dotnet).Source
        $runtime = Get-ChildItem -LiteralPath (Join-Path $dotnetRoot 'shared/Microsoft.NETCore.App') -Directory |
            Where-Object Name -match '^10\.\d+\.\d+$' | Sort-Object { [version]$_.Name } | Select-Object -Last 1
        if (-not $runtime) { Fail 'no .NET 10 runtime installed to ship (or pass -NoDotNet)' }
        $fxr = Join-Path $dotnetRoot "host/fxr/$($runtime.Name)"
        if (-not (Test-Path -LiteralPath (Join-Path $fxr 'hostfxr.dll'))) { Fail "no hostfxr for .NET $($runtime.Name) at $fxr" }
        Write-Host "Laying out .NET $($runtime.Name)..."
        $madeDotNet = $true
        New-Item -ItemType Directory -Force -Path (Join-Path $dotNetFolder 'host/fxr'), (Join-Path $dotNetFolder 'shared/Microsoft.NETCore.App') | Out-Null
        Copy-Item -LiteralPath (Join-Path $dotnetRoot 'dotnet.exe') -Destination $dotNetFolder
        Copy-Item -LiteralPath $fxr -Destination (Join-Path $dotNetFolder 'host/fxr') -Recurse
        Copy-Item -LiteralPath $runtime.FullName -Destination (Join-Path $dotNetFolder 'shared/Microsoft.NETCore.App') -Recurse
        foreach ($notice in 'LICENSE.txt', 'ThirdPartyNotices.txt') {
            Copy-Item -LiteralPath (Join-Path $dotnetRoot $notice) -Destination $dotNetFolder
        }
    }

    # --- Build ----------------------------------------------------------------------------------------------------
    # UBT is built here, not by BuildCookRun: one UBT runs at a time on the machine, and only UBT's own -WaitMutex
    # waits for another (an editor's Live Coding, another checkout) instead of failing.
    $ubt = Join-Path $Engine 'Engine/Build/BatchFiles/Build.bat'
    Write-Host 'Building the editor (the cook runs it)...'
    Invoke-Native 'Build (editor)' { & $ubt OpenSCCTEditor Win64 Development "-project=$project" -WaitMutex *> (Join-Path $Out 'build-editor.log') }
    # The game is built from a drive letter that stands for the checkout (subst), not from the checkout's own path:
    # the compiler writes each source file's full path into the game (__FILE__, which Slate's SNew records), and the
    # checkout's path holds the user name of whoever builds it. From the drive the game has X:\Source\... instead.
    # The editor is built where it lives: only the game ships. The receipt names its files from $(ProjectDir), so
    # staging from the checkout's path afterwards finds them.
    # The letter is in those paths too, so it is always the same one (-BuildDrive): the same code then builds the same
    # exe, and an unchanged exe costs a player nothing to update.
    $drive = "$($BuildDrive.ToUpperInvariant()):"
    $mapped = @(subst | Where-Object { $_ -like "$drive\*" })
    $ours = $mapped.Count -gt 0 -and $mapped[0] -match '=>\s*(.+)$' -and ([IO.Path]::GetFullPath($Matches[1].Trim()).TrimEnd('\') -eq $repo.TrimEnd('\'))
    if (-not $ours) {
        if ($mapped.Count -gt 0 -or (Test-Path -LiteralPath "$drive\")) { Fail "the drive $drive is in use: free it or pass -BuildDrive with another letter (always the same one)" }
        Invoke-Native "subst $drive" { subst $drive $repo }
    }
    try {
        Write-Host "Building the game ($Configuration) from $drive..."
        $driveProject = "$drive\OpenSCCTUnreal.uproject"
        Invoke-Native 'Build (game)' { & $ubt $target Win64 $Configuration "-project=$driveProject" -WaitMutex -NoUBTMakefiles *> (Join-Path $Out 'build-game.log') }
    } finally {
        if (-not $ours) { subst $drive /D | Out-Null }
    }

    # --- Cook, stage, archive --------------------------------------------------------------------------------------
    Write-Host "Cooking and staging... this takes a while; the log is $Out\uat.log"
    $uatArgs = @(
        # -WaitForUATMutex: one AutomationTool runs at a time per engine; wait for another checkout's instead of failing.
        '-WaitForUATMutex', 'BuildCookRun', "-project=$project", '-noP4', '-utf8output', '-unattended',
        '-platform=Win64', "-clientconfig=$Configuration", "-target=$target", '-nocompileeditor',
        '-cook', '-stage', '-pak', '-iostore', '-compressed', '-nodebuginfo',
        '-archive', "-archivedirectory=$archive",
        # A map list on the command line replaces MapsToCook in Config/DefaultGame.ini, hence the front end and the
        # runtime level (which builds every map without an imported level from the Steam conversion) too.
        ('-MapsToCook=' + ((@('/Game/OpenSCCT/Maps/L_FrontEnd', '/Game/OpenSCCT/RuntimeMap/L_RuntimeMap') +
            @($mapsToCook | ForEach-Object { "/Game/Imported/Maps/$_/$_" })) -join '+'))
    )
    if ($Configuration -eq 'Shipping') { $uatArgs += '-distribution' }
    & $uat @uatArgs *> (Join-Path $Out 'uat.log')
    if ($LASTEXITCODE -ne 0) { Fail "BuildCookRun failed (exit code $LASTEXITCODE; see $Out\uat.log)" }

    # UAT normally adds Windows under the archive, but can archive directly when a path segment starts with Win64 or Windows.
    $staged = Join-Path $archive 'Windows'
    if (-not (Test-Path -LiteralPath (Join-Path $staged $starter))) { $staged = $archive }
    if (-not (Test-Path -LiteralPath (Join-Path $staged $starter))) { Fail "the archive has no $starter (see $Out\uat.log)" }
    if ($madeDotNet) {
        $stagedRuntime = Join-Path $staged "$target/Binaries/DotNet"
        # UAT may also have staged a developer's runtime through the target receipt. The package uses its selected version.
        if (Test-Path -LiteralPath $stagedRuntime) { Remove-Item -LiteralPath $stagedRuntime -Recurse -Force }
        Move-Item -LiteralPath $dotNetFolder -Destination $stagedRuntime
    }
} finally {
    if ($madeDotNet -and (Test-Path -LiteralPath $dotNetFolder)) { Remove-Item -LiteralPath $dotNetFolder -Recurse -Force }
}

& (Join-Path $PSScriptRoot 'Test/Assert-ReleaseContent.ps1') -Stage $staged -OpenScct $runtimeRoot -Engine $Engine -Report (Join-Path $Out 'cook-audit.json') -ImportedMaps $mapsToCook
# Map pak cooks are for local development. Release builds load player-prepared maps at runtime
# and use the default cook configuration, with a single project chunk.
$stagedPaks = Join-Path $staged "$target/Content/Paks"
$chunks = @(Get-ChildItem -LiteralPath $stagedPaks -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^pakchunk(\d+)' -and [int]$Matches[1] -ne 0 } | ForEach-Object Name)
if ($chunks.Count -gt 0) { Fail "the release cook made map pak containers ($($chunks -join ', ')): it must not read Config/Custom/MapPaks" }
Get-ChildItem -LiteralPath $staged -File -Filter 'Manifest_*.txt' | Remove-Item -Force

Move-Item -LiteralPath $staged -Destination $folder
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Recurse -Force }

# Debug files of the bridge and anything else that slipped through.
Get-ChildItem -LiteralPath $folder -Recurse -File -Include '*.pdb', '*.debug', '*.sym' | Remove-Item -Force
$prerequisites = Join-Path $folder 'Prerequisites'
New-Item -ItemType Directory -Force -Path $prerequisites | Out-Null
Copy-Item -LiteralPath $vcRedist -Destination $prerequisites

# --- Documents -----------------------------------------------------------------------------------------------------
$utf8 = [Text.UTF8Encoding]::new($false)
function Write-Text([string]$path, [string]$text) {
    $crlf = ($text -replace "`r`n", "`n") -replace "`n", "`r`n"
    [IO.File]::WriteAllText($path, $crlf, $utf8)
}
$title = "$product $Version"
$readme = @(
    $title, ('=' * $title.Length), '',
    'OpenSCCT on Unreal Engine 5: the OpenSCCT simulation and servers with an Unreal presentation.',
    'Run OpenSCCTUnreal.exe to start the game.', '',
    'Choose Set up from Steam in the Unreal front end and select your installation of Splinter Cell Chaos Theory.',
    'Unreal loads the stock Versus maps, characters, sounds and equipment from that local build.', '',
    'If the game does not start, install the Visual C++ runtime: Prerequisites\vc_redist.x64.exe.',
    'It needs a DirectX 12 graphics card with Shader Model 6.', '',
    'OpenSCCT is a fan project. It is not affiliated with or endorsed by Ubisoft. Splinter Cell is a trademark of',
    'Ubisoft Entertainment.', '',
    'Notices', '-------', '',
    'OpenSCCT Unreal uses the Unreal(R) Engine. Unreal(R) is a trademark or registered trademark of Epic Games, Inc. in',
    'the United States of America and elsewhere. Unreal(R) Engine, Copyright 1998 - 2026, Epic Games, Inc. All rights',
    'reserved.', '',
    "This Unreal build includes its own simulation and content tools. Source: https://github.com/OpenSCCT/OpenSCCT-Unreal/tree/$commit", '',
    'The textures from ambientCG (ambientcg.com) and Poly Haven (polyhaven.com) are CC0.', '',
    'THIRD-PARTY-NOTICES.md lists the third-party components this release ships and their licences.'
)
if (-not $NoDotNet) {
    $readme += @('', 'The .NET runtime in OpenSCCTUnreal\Binaries\DotNet is (c) the .NET Foundation and contributors, under the MIT',
        'licence: see LICENSE.txt and ThirdPartyNotices.txt in that folder.')
}
Write-Text (Join-Path $folder 'README.txt') ($readme -join "`n")
# The repository's list of the third-party components a release ships, with their licences.
Copy-Item -LiteralPath (Join-Path $repo 'THIRD-PARTY-NOTICES.md') -Destination $folder
& (Join-Path $PSScriptRoot 'Copy-GraphicsLicences.ps1') -Package $folder -Engine $Engine
Copy-Item -LiteralPath (Join-Path $plugin 'LICENSE') -Destination (Join-Path $folder 'glTFRuntime-LICENSE.txt')

if (-not $NoDotNet) {
    Write-Host 'Packaging standalone Steam content setup...'
    Invoke-Native 'Unreal content tools' { python (Join-Path $PSScriptRoot 'UnrealContent/package.py') (Join-Path $folder 'contenttools') }
}

# --- version.json --------------------------------------------------------------------------------------------------
$entries = @(Get-ChildItem -LiteralPath $folder -Force | ForEach-Object Name) + 'version.json' | Sort-Object -Unique
Write-Host 'Hashing...'
$files = @(Get-ChildItem -LiteralPath $folder -Recurse -File -Force | Where-Object { $_.FullName -ne (Join-Path $folder 'version.json') } |
    Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path   = $_.FullName.Substring($folder.Length + 1).Replace('\', '/')
            size   = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
$handshake = [ordered]@{
    launcherProtocol = 1
    product          = $product
    # Older launchers display these fields. Source revisions now belong to the Unreal repository.
    openscctVersion  = $runtimeVersion
    openscctCommit   = $commit
}
$info = [ordered]@{
    format          = 1
    product         = $product
    version         = $Version
    commit          = $commit
    platform        = $platform
    configuration   = $Configuration
    engineVersion   = $engineVersion
    launcherProtocol = 1
    editorMapProtocol = 1
    openscctVersion = $runtimeVersion
    openscctCommit  = $commit
    executable      = $starter
    gameExecutable  = "$target/Binaries/Win64/$gameExe"
    runtimeTree     = $runtimeTree
    contentSource   = 'local-steam'
    bundledMaps     = @($bundledMaps)
    built          = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    entries         = $entries
    files           = $files
}
[IO.File]::WriteAllText((Join-Path $folder 'version.json'), ($info | ConvertTo-Json -Depth 4) + "`n", $utf8)

# --- Checks --------------------------------------------------------------------------------------------------------
# NOTICES.txt is the engine's own third-party notices, staged by UAT.
$expected = @($starter, $target, 'Engine', 'NOTICES.txt', 'Prerequisites', 'README.txt', 'THIRD-PARTY-NOTICES.md', 'glTFRuntime-LICENSE.txt', 'version.json')
if (-not $NoDotNet) { $expected += 'contenttools' }
$unexpected = $entries | Where-Object { $_ -notin $expected }
$missing = $expected | Where-Object { $_ -notin $entries }
if ($unexpected) { Warn-Or-Fail "unexpected entries in the folder: $($unexpected -join ', ')" }
if ($missing) { Fail "missing from the folder: $($missing -join ', ')" }
foreach ($needed in @("$target/Binaries/Win64/$gameExe", "$target/Binaries/Bridge/OpenScct.UnrealBridge.dll",
        "$target/Binaries/Bridge/OpenScct.UnrealBridge.runtimeconfig.json")) {
    if (-not (Test-Path -LiteralPath (Join-Path $folder $needed))) { Fail "the package has no $needed" }
}
if (-not $NoDotNet -and -not (Get-ChildItem -LiteralPath (Join-Path $folder "$target/Binaries/DotNet/host/fxr") -Recurse -Filter hostfxr.dll -ErrorAction SilentlyContinue)) {
    Fail 'the package has no private .NET runtime (Binaries/DotNet was not staged)'
}

# No path of the machine that built it (a user name, a checkout) may ship inside a program file or a text file. The
# cooked paks are left out: they are large and hold no paths of their own.
$needles = @($env:USERPROFILE, $repo) | Where-Object { $_ } | ForEach-Object { $_; $_.Replace('\', '/') } | Sort-Object -Unique
$latin1 = [Text.Encoding]::Latin1
$leaks = foreach ($file in Get-ChildItem -LiteralPath $folder -Recurse -File -Include '*.exe', '*.dll', '*.json', '*.ini', '*.txt', '*.xml') {
    $text = $latin1.GetString([IO.File]::ReadAllBytes($file.FullName))
    foreach ($needle in $needles) {
        $wide = $latin1.GetString([Text.Encoding]::Unicode.GetBytes($needle))
        if ($text.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or $text.IndexOf($wide, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            "$($file.FullName.Substring($folder.Length + 1)): $needle"
        }
    }
}
if ($leaks) { Warn-Or-Fail ("private paths inside the package:`n  " + (($leaks | Sort-Object -Unique) -join "`n  ")) }

# --- Zip and checksums ---------------------------------------------------------------------------------------------
Write-Host 'Zipping...'
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($folder, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
$zipSize = (Get-Item -LiteralPath $zip).Length
# The launcher installs from the file list and its per-file blobs, so a release may leave out a zip that is too big
# for one GitHub asset. Each blob is still checked against the limit below.
$uploadZip = $zipSize -le $assetLimit
if (-not $uploadZip) { Write-Warning ('the zip is {0:0.00} GB, over what one GitHub release asset may weigh: the release installs through the launcher only' -f ($zipSize / 1GB)) }
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
$line = "$hash  $zipName`n"
[IO.File]::WriteAllText("$zip.sha256", $line, $utf8)
[IO.File]::WriteAllText((Join-Path $Out 'SHA256SUMS'), $(if ($uploadZip) { $line } else { '' }), $utf8)
[IO.File]::WriteAllText((Join-Path $Out $launcherName), ($handshake | ConvertTo-Json) + "`n", $utf8)

# --- Delta update files --------------------------------------------------------------------------------------------
# The launcher's game format: every file with its SHA-256 and the asset that holds it. A file some base release already
# published keeps pointing at that asset; a new one of 256 KB or more is its own <sha256>.gz, smaller new ones share
# one zip pack (entries named by their SHA-256), so a release stays well under GitHub's 1000 assets.
New-Item -ItemType Directory -Force -Path $storeDir | Out-Null
# A blob is an ordered table when made here, an object when read from a base release's list.
function Test-Packed($blob) { if ($blob -is [Collections.IDictionary]) { $blob.Contains('member') } else { [bool]$blob.PSObject.Properties['member'] -and [bool]$blob.member } }
$known = @{}
# Older stores include mixed small-file archives and retail-bearing containers. Fresh installs use only the
# audited asset channel. Existing players can still reuse byte-identical local files and binary deltas.
foreach ($b in $bases | Where-Object SteamAssets) {
    foreach ($f in $b.List) {
        if (-not $known.ContainsKey($f.sha256)) { $known[$f.sha256] = $f.blob }
    }
}
$packLimit = 256KB
$packPath = Join-Path $storeDir $packName
$pack = $null
$newBlobs = @{}
$listEntries = [System.Collections.Generic.List[object]]::new()
Write-Host 'Writing the delta update files...'
try {
    foreach ($f in $files | Where-Object { $_.path -ne 'version.json' }) {
        $full = Join-Path $folder $f.path
        $blob = $known[$f.sha256]
        if (-not $blob) { $blob = $newBlobs[$f.sha256] }
        if (-not $blob) {
            if ($f.size -lt $packLimit) {
                if (-not $pack) { $pack = [IO.Compression.ZipFile]::Open($packPath, [IO.Compression.ZipArchiveMode]::Create) }
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($pack, $full, $f.sha256, [IO.Compression.CompressionLevel]::Optimal)
                $blob = [ordered]@{ tag = $StoreTag; name = $packName; size = 0; member = $f.sha256 }
            } else {
                $gz = Join-Path $storeDir "$($f.sha256).gz"
                $in = [IO.File]::OpenRead($full); $outStream = [IO.File]::Create($gz)
                $gzip = [IO.Compression.GZipStream]::new($outStream, [IO.Compression.CompressionLevel]::Optimal)
                try { $in.CopyTo($gzip) } finally { $gzip.Dispose(); $outStream.Dispose(); $in.Dispose() }
                $blob = [ordered]@{ tag = $StoreTag; name = "$($f.sha256).gz"; size = (Get-Item -LiteralPath $gz).Length }
            }
            $newBlobs[$f.sha256] = $blob
        }
        $listEntries.Add([ordered]@{ path = $f.path; size = $f.size; sha256 = $f.sha256; blob = $blob })
    }
    # version.json last: it changes every build (its time stamp), and it is small.
    $vj = Get-Item -LiteralPath (Join-Path $folder 'version.json')
    $vjSha = (Get-FileHash -LiteralPath $vj.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if (-not $pack) { $pack = [IO.Compression.ZipFile]::Open($packPath, [IO.Compression.ZipArchiveMode]::Create) }
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($pack, $vj.FullName, $vjSha, [IO.Compression.CompressionLevel]::Optimal)
    $vjBlob = [ordered]@{ tag = $StoreTag; name = $packName; size = 0; member = $vjSha }
    $newBlobs[$vjSha] = $vjBlob
    $listEntries.Add([ordered]@{ path = 'version.json'; size = $vj.Length; sha256 = $vjSha; blob = $vjBlob })
} finally {
    if ($pack) { $pack.Dispose() }
}
if (Test-Path -LiteralPath $packPath) {
    # Named by its contents too: a version packaged again gets a new pack instead of one the store already has under
    # the same name with other members.
    $packSha = (Get-FileHash -LiteralPath $packPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $finalPack = $packName -replace '\.zip$', "-$($packSha.Substring(0, 16)).zip"
    Move-Item -LiteralPath $packPath -Destination (Join-Path $storeDir $finalPack)
    $packSize = (Get-Item -LiteralPath (Join-Path $storeDir $finalPack)).Length
    foreach ($b in $newBlobs.Values) { if ($b.Contains('member')) { $b.size = $packSize; $b.name = $finalPack } }
}

# Patches: for each file of 1 MB or more that a base release has at the same path with other contents, a binary patch
# from that version of it, kept when it is under half the gzipped file. Named by both hashes, so the same patch is
# never made or uploaded twice. Every patch is applied once here and checked against the file's SHA-256.
# Large IoStore containers reuse compressed blocks by hash. Keep the suffix-array cap for other large files.
$patchMin = 1MB
$patchMax = 256MB
$jobs = @{}
foreach ($e in $listEntries) {
    if ($e.size -lt $patchMin) { continue }
    foreach ($b in $bases) {
        if (!$b.SteamAssets -and $e.path -match '\.(pak|utoc|ucas)$') { continue }
        $old = $b.Files | Where-Object path -eq $e.path | Select-Object -First 1
        if (-not $old -or $old.sha256 -eq $e.sha256) { continue }
        $method = 'diff'
        if ($e.size -gt $patchMax -or $old.size -gt $patchMax) {
            if ($e.path -notlike '*.ucas') { continue }
            $method = 'diff-iostore'
        }
        $name = "$($old.sha256)-$($e.sha256).osdiff"
        if (-not $jobs.ContainsKey($name)) {
            $jobs[$name] = [pscustomobject]@{ Name = $name; Method = $method; Path = $e.path; From = (Join-Path $b.Folder $old.path); FromSha = $old.sha256
                FromSize = $old.size; To = (Join-Path $folder $e.path); Patch = (Join-Path $storeDir $name) }
        }
    }
}
if ($jobs.Count -gt 0) {
    Write-Host "Making $($jobs.Count) patches..."
    $failures = $jobs.Values | ForEach-Object -ThrottleLimit 4 -Parallel {
        $check = "$($_.Patch).check"
        & $using:deltaTool $_.Method $_.From $_.To $_.Patch 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 3 -and $_.Method -eq 'diff-iostore') {
            Write-Warning "No supported IoStore block table for $($_.Path) from $($_.FromSha); using its full download."
            return
        }
        if ($LASTEXITCODE -ne 0) { "$($_.Path): diff failed"; return }
        & $using:deltaTool apply $_.From $_.Patch $check 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { "$($_.Path): the patch does not apply"; return }
        Remove-Item -LiteralPath $check -Force
    }
    if ($failures) { Fail ("patches failed:`n  " + ($failures -join "`n  ")) }
}
$patchBytes = 0L
foreach ($e in $listEntries) {
    $refs = @(foreach ($j in $jobs.Values | Where-Object Path -eq $e.path) {
        if (-not (Test-Path -LiteralPath $j.Patch)) { continue }
        $size = (Get-Item -LiteralPath $j.Patch).Length
        $full = if (Test-Packed $e.blob) { $e.size } else { $e.blob.size }
        if ($size -ge $full / 2) { Remove-Item -LiteralPath $j.Patch -Force; continue }
        $patchBytes += $size
        [ordered]@{ from = $j.FromSha; fromSize = $j.FromSize; tag = $StoreTag; name = $j.Name; size = $size }
    })
    if ($refs.Count -gt 0) { $e['patches'] = $refs }
}

$listPath = Join-Path $storeDir $listName
[IO.File]::WriteAllText($listPath, ([ordered]@{ format = 1; version = $Version; commit = $commit; files = $listEntries } | ConvertTo-Json -Depth 6) + "`n", $utf8)
[IO.File]::WriteAllText("$listPath.sha256", "$((Get-FileHash -LiteralPath $listPath -Algorithm SHA256).Hash.ToLowerInvariant())  $listName`n", $utf8)
$uploads = @(Get-ChildItem -LiteralPath $storeDir -File | Where-Object { $_.Name -notlike '*-files.json*' })
$heavy = @($uploads | Where-Object Length -gt $assetLimit | ForEach-Object Name)
if ($heavy) { Warn-Or-Fail ("store files over what one GitHub release asset may weigh:`n  " + ($heavy -join "`n  ")) }
Write-Host ('{0} files; {1} new blobs ({2:N1} MB), {3} patches ({4:N1} MB)' -f $listEntries.Count, $newBlobs.Count,
    ((($uploads | Where-Object Extension -ne '.osdiff') | Measure-Object Length -Sum).Sum / 1MB), @($jobs.Values | Where-Object { Test-Path -LiteralPath $_.Patch }).Count, ($patchBytes / 1MB))
foreach ($b in $bases) {
    # What a player on that base downloads: each blob (a pack once, whole) holding a file it lacks; with patches, the
    # patch instead where one applies to the file it has.
    $have = @{}; foreach ($f in $b.Files) { $have[$f.sha256] = $true }
    $plain = @{}; $patched = @{}
    foreach ($e in $listEntries) {
        if ($have.ContainsKey($e.sha256)) { continue }
        $blobKey = "$($e.blob.tag)/$($e.blob.name)"
        $plain[$blobKey] = $e.blob.size
        $p = @($e['patches']) | Where-Object { $_ -and $have.ContainsKey($_.from) } | Sort-Object size | Select-Object -First 1
        if ($p) { $patched["$($p.tag)/$($p.name)"] = $p.size } else { $patched[$blobKey] = $e.blob.size }
    }
    $sum = { param($t) ($t.Values | Measure-Object -Sum).Sum / 1MB }
    Write-Host ('  from {0}: {1:N1} MB with patches, {2:N1} MB without' -f $b.Version, (& $sum $patched), (& $sum $plain))
}

# --- Notes and the publish command ---------------------------------------------------------------------------------
$previous = git -C $repo describe --tags --abbrev=0 --match 'v*' HEAD 2>$null
$log = if ($LASTEXITCODE -eq 0 -and $previous) { git -C $repo log --no-merges "$previous..HEAD" --format='- %s' } else { @('- First release.') }
$notes = @($title, '', "Includes the Unreal-owned simulation ($runtimeVersion) and local content setup.", '',
    '## Changes', '') + $log + @('', '## Maps', '',
    'Open the Unreal front end and choose Set up from Steam to build your local content.', '', '## Download', '',
    $(if ($uploadZip) { 'Download the Windows game ZIP, extract it and run OpenSCCTUnreal.exe.' } else { 'Install and update through the OpenSCCT launcher: the Windows game is too large for a single download here.' }) +
    ' The Linux and Windows server archives run independently.')
Write-Text (Join-Path $Out 'notes.md') ($notes -join "`n")

if (-not $Local) {
    & (Join-Path $PSScriptRoot 'Package-DedicatedServer.ps1') -Version $Version -Out $Out | Out-Host
    $serverNames = @("OpenSCCT-Unreal-Server-$Version-linux-x64.tar.gz", "OpenSCCT-Unreal-Server-$Version-win-x64.zip")
    foreach ($name in $serverNames) {
        $checksum = (Get-Content -Raw -LiteralPath (Join-Path $Out "$name.sha256")).Trim()
        [IO.File]::AppendAllText((Join-Path $Out 'SHA256SUMS'), "$checksum`n", $utf8)
    }
}
if (-not $Local -and $Repository) {
    $pre = if ($isPrerelease) { ' --prerelease' } else { '' }
    $publish = @(
        '# Uploads the blobs and patches to the shared store release, then this package as a DRAFT release. Review the',
        '# draft on GitHub, then publish it there. Blobs and patches are named by their hashes: uploading one again is harmless.',
        '$ErrorActionPreference = ''Stop''',
        "`$store = Join-Path `$PSScriptRoot 'unreal-files'",
        "gh release view $StoreTag -R $Repository *> `$null",
        "if (`$LASTEXITCODE -ne 0) { gh release create $StoreTag -R $Repository --prerelease --title 'OpenSCCT Unreal files ($StoreTag)' --notes 'The files the OpenSCCT launcher installs and updates OpenSCCT Unreal from. Not a release to download.'; if (`$LASTEXITCODE -ne 0) { throw 'could not make $StoreTag' } }",
        "`$have = @((gh release view $StoreTag -R $Repository --json assets | ConvertFrom-Json).assets | ForEach-Object name)",
        "`$new = @(Get-ChildItem -LiteralPath `$store -File | Where-Object { `$_.Name -notlike '*-files.json*' -and `$_.Name -notin `$have } | ForEach-Object FullName)",
        "if (`$have.Count + `$new.Count -gt 990) { throw `"$StoreTag would pass GitHub's 1000 assets per release (`$(`$have.Count) there, `$(`$new.Count) new): start unreal-files-<n+1> with -StoreTag.`" }",
        "if (`$new.Count -gt 0) { gh release upload $StoreTag -R $Repository @new; if (`$LASTEXITCODE -ne 0) { throw 'upload to $StoreTag failed' } }",
        '# Older launchers locate update metadata in unreal-files-1. The file list names each clean blob store.',
        "`$locator = 'unreal-files-1'",
        "`$metadata = @((Join-Path `$store '$listName'), (Join-Path `$store '$listName.sha256'))",
        "`$locatorAssets = @((gh release view `$locator -R $Repository --json assets | ConvertFrom-Json).assets)",
        "if (`$LASTEXITCODE -ne 0) { throw 'Cannot read the launcher metadata release.' }",
        "foreach (`$file in `$metadata) {",
        "    `$name = Split-Path -Leaf `$file",
        "    `$asset = `$locatorAssets | Where-Object name -eq `$name | Select-Object -First 1",
        "    if (`$asset) {",
        "        `$sha = 'sha256:' + (Get-FileHash -LiteralPath `$file).Hash.ToLowerInvariant()",
        "        if (`$asset.digest -ne `$sha) { throw `"Existing launcher metadata differs: `$name`" }",
        "    } else {",
        "        gh release upload `$locator -R $Repository `$file",
        "        if (`$LASTEXITCODE -ne 0) { throw `"Cannot upload launcher metadata `$name.`" }",
        "    }",
        "}",
        "gh release create v$Version -R $Repository --target $commit --draft --title '$title' --notes-file (Join-Path `$PSScriptRoot 'notes.md')$pre ``",
        "    $(if ($uploadZip) { "(Join-Path `$PSScriptRoot '$zipName') (Join-Path `$PSScriptRoot '$zipName.sha256') " })(Join-Path `$PSScriptRoot 'SHA256SUMS') (Join-Path `$PSScriptRoot '$launcherName') ``",
        "    (Join-Path `$store '$listName') (Join-Path `$store '$listName.sha256') ``",
        "    (Join-Path `$PSScriptRoot '$($serverNames[0])') (Join-Path `$PSScriptRoot '$($serverNames[0]).sha256') ``",
        "    (Join-Path `$PSScriptRoot '$($serverNames[1])') (Join-Path `$PSScriptRoot '$($serverNames[1]).sha256')",
        "if (`$LASTEXITCODE -ne 0) { throw 'creating the draft release failed' }",
        '# The next release makes its patches from this one and reuses its blobs: only once they are on GitHub.',
        "Set-Content -LiteralPath (Join-Path `$PSScriptRoot '.published') -Value 'Uploaded to $Repository ($StoreTag, v$Version).'"
    )
    Write-Text (Join-Path $Out 'publish.ps1') ($publish -join "`n")
}

Write-Host ''
Write-Host ('Packaged {0} ({1:0.0} MB) in {2}' -f $zipName, ($zipSize / 1MB), $Out)
if ($Local) { Write-Host 'A local package: try it out, never upload it.' }
elseif ($Repository) { Write-Host "Next: tag v$Version, edit notes.md, then run $(Join-Path $Out 'publish.ps1') to make the draft release." }
else { Write-Host 'No -Repository: no publish.ps1 was written.' }

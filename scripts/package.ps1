param(
    [string]$Version,
    [string]$BuildRoot,
    [string]$OutputRoot
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$ProgramId = "010099C022B96000"
$ExpectedExlaunchCommit = "f698816d6e198afb0029ad5c07d55e7017a620fe"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot "VERSION")).Trim()
}
if ([string]::IsNullOrWhiteSpace($BuildRoot)) {
    $BuildRoot = Join-Path $RepoRoot "artifacts\build"
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot "artifacts\package"
}
$BuildRoot = (Resolve-Path -LiteralPath $BuildRoot).Path

function Write-Utf8 {
    param([string]$Path, [string]$Content)
    [IO.File]::WriteAllText($Path, $Content, $Utf8NoBom)
}

function Get-RelativeUnix {
    param([string]$Root, [string]$Path)
    return $Path.Substring($Root.Length).TrimStart('\', '/').Replace('\', '/')
}

function Assert-ChildPath {
    param([string]$Root, [string]$Path)
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($fullRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes package output root: $fullPath"
    }
    return $fullPath
}

$buildReportPath = Join-Path $BuildRoot "build-report.json"
$subsdkPath = Join-Path $BuildRoot "subsdk9"
$npdmPath = Join-Path $BuildRoot "main.npdm"
foreach ($path in @($buildReportPath, $subsdkPath, $npdmPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Build first; missing $path"
    }
}
$buildReport = Get-Content -Raw -LiteralPath $buildReportPath | ConvertFrom-Json
if ($buildReport.status -ne "PASS" -or $buildReport.version -ne $Version -or $buildReport.exlaunch_commit -ne $ExpectedExlaunchCommit) {
    throw "Build report does not match the requested release."
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$packageName = "MarioGalaxyLocalCoop-$Version"
$stageRoot = Join-Path $OutputRoot $packageName
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath (Assert-ChildPath -Root $OutputRoot -Path $stageRoot) -Recurse -Force
}
$contentRoot = Join-Path $stageRoot "payload\contents\$ProgramId"
New-Item -ItemType Directory -Force -Path (Join-Path $contentRoot "exefs") | Out-Null

Copy-Item -LiteralPath $subsdkPath -Destination (Join-Path $contentRoot "exefs\subsdk9")
Copy-Item -LiteralPath $npdmPath -Destination (Join-Path $contentRoot "exefs\main.npdm")
Copy-Item -LiteralPath (Join-Path $RepoRoot "scripts\install.ps1") -Destination (Join-Path $stageRoot "install.ps1")

$payloadRecords = @()
foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $stageRoot "payload") -Recurse -File | Sort-Object FullName)) {
    $payloadRecords += [ordered]@{
        path = Get-RelativeUnix -Root $stageRoot -Path $file.FullName
        bytes = $file.Length
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
    }
}

$packageManifest = [ordered]@{
    schema_version = 1
    name = "Super Mario Galaxy Local Co-op"
    version = $Version
    program_id = $ProgramId
    primary_target = "Atmosphere"
    exlaunch_commit = $ExpectedExlaunchCommit
    files = $payloadRecords
}
$manifestPath = Join-Path $stageRoot "PACKAGE_MANIFEST.json"
Write-Utf8 -Path $manifestPath -Content (($packageManifest | ConvertTo-Json -Depth 8) + "`n")

$checksumFiles = @(Get-ChildItem -LiteralPath $stageRoot -Recurse -File | Where-Object { $_.Name -ne "CHECKSUMS.txt" } | Sort-Object FullName)
$checksumLines = @()
foreach ($file in $checksumFiles) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
    $checksumLines += "$hash *$(Get-RelativeUnix -Root $stageRoot -Path $file.FullName)"
}
Write-Utf8 -Path (Join-Path $stageRoot "CHECKSUMS.txt") -Content (($checksumLines -join "`n") + "`n")

$zipPath = Join-Path $OutputRoot "$packageName.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath (Assert-ChildPath -Root $OutputRoot -Path $zipPath) -Force
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in @(Get-ChildItem -LiteralPath $stageRoot -Recurse -File | Sort-Object FullName)) {
        $entryName = "$packageName/" + (Get-RelativeUnix -Root $stageRoot -Path $file.FullName)
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $entryName, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $zip.Dispose()
}

$zipHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash
$sidecarPath = "$zipPath.sha256"
Write-Utf8 -Path $sidecarPath -Content "$zipHash *$packageName.zip`n"

$packageReport = [ordered]@{
    schema_version = 1
    status = "PASS"
    version = $Version
    program_id = $ProgramId
    exlaunch_commit = $ExpectedExlaunchCommit
    zip = [ordered]@{
        path = $zipPath
        bytes = (Get-Item -LiteralPath $zipPath).Length
        sha256 = $zipHash
    }
    sidecar = $sidecarPath
    files = $payloadRecords
}
Write-Utf8 -Path (Join-Path $OutputRoot "package-report.json") -Content (($packageReport | ConvertTo-Json -Depth 8) + "`n")
Write-Host "Package ready: $zipPath ($zipHash)"

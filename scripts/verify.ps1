param(
    [string]$BuildRoot,
    [string]$PackageRoot,
    [string]$ReportPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$ExpectedVersion = (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot "VERSION")).Trim()
$ExpectedCommit = "f698816d6e198afb0029ad5c07d55e7017a620fe"
$ProgramId = "010099C022B96000"
$ExpectedDockerImage = "devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282"

if ([string]::IsNullOrWhiteSpace($BuildRoot)) { $BuildRoot = Join-Path $RepoRoot "artifacts\build" }
if ([string]::IsNullOrWhiteSpace($PackageRoot)) { $PackageRoot = Join-Path $RepoRoot "artifacts\package" }
if ([string]::IsNullOrWhiteSpace($ReportPath)) { $ReportPath = Join-Path $RepoRoot "artifacts\verify\verification-report.json" }
$VerifyRoot = Split-Path -Parent $ReportPath
$Utf8NoBom = New-Object Text.UTF8Encoding($false)

$buildReport = Get-Content -Raw -LiteralPath (Join-Path $BuildRoot "build-report.json") | ConvertFrom-Json
if ($buildReport.status -ne "PASS" -or $buildReport.version -ne $ExpectedVersion -or $buildReport.program_id -ne $ProgramId -or $buildReport.exlaunch_commit -ne $ExpectedCommit -or -not $buildReport.clean_build) {
    throw "Build report failed release identity checks."
}
$subsdk = Join-Path $BuildRoot "subsdk9"
$npdm = Join-Path $BuildRoot "main.npdm"
$elf = Join-Path $BuildRoot "exlaunch.elf"
foreach ($path in @($subsdk, $npdm, $elf)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing build output: $path"
    }
}
$reportedSubsdk = @($buildReport.outputs | Where-Object { $_.path -eq "artifacts/build/subsdk9" })
if ($reportedSubsdk.Count -ne 1 -or (Get-FileHash -Algorithm SHA256 -LiteralPath $subsdk).Hash -ne $reportedSubsdk[0].sha256) {
    throw "Built subsdk9 does not match its report."
}
$reportedNpdm = @($buildReport.outputs | Where-Object { $_.path -eq "artifacts/build/main.npdm" })
if ($reportedNpdm.Count -ne 1 -or (Get-FileHash -Algorithm SHA256 -LiteralPath $npdm).Hash -ne $reportedNpdm[0].sha256) {
    throw "Built main.npdm does not match its report."
}

$workflowText = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot ".github\workflows\build.yml")
$buildScriptText = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot "scripts\build.ps1")
if ($workflowText -notmatch '\./scripts/build\.ps1 -UseDocker' -or
    -not $buildScriptText.Contains($ExpectedDockerImage)) {
    throw "CI workflow or container dependency is not configured as expected."
}

$forbiddenBinaryPatterns = @(
    '(?i)[A-Z]:[\\/]+Users[\\/]+',
    '(?i)MarioGalaxy1\+2Switch|dev_private'
)
foreach ($binary in @($elf, $subsdk)) {
    $binaryText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($binary))
    foreach ($pattern in $forbiddenBinaryPatterns) {
        if ([regex]::IsMatch($binaryText, $pattern)) {
            throw "Release binary $([IO.Path]::GetFileName($binary)) contains forbidden pattern: $pattern"
        }
    }
}

$packageReport = Get-Content -Raw -LiteralPath (Join-Path $PackageRoot "package-report.json") | ConvertFrom-Json
if ($packageReport.status -ne "PASS" -or $packageReport.version -ne $ExpectedVersion -or $packageReport.program_id -ne $ProgramId) {
    throw "Package report failed release identity checks."
}
$zipPath = [string]$packageReport.zip.path
if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf) -or (Get-FileHash -Algorithm SHA256 -LiteralPath $zipPath).Hash -ne $packageReport.zip.sha256) {
    throw "ZIP hash does not match its package report."
}
$sidecar = Get-Content -Raw -LiteralPath "$zipPath.sha256"
if ($sidecar -notmatch '^([0-9A-Fa-f]{64}) \*') {
    throw "ZIP checksum sidecar is malformed."
}
if ($Matches[1].ToUpperInvariant() -ne $packageReport.zip.sha256) {
    throw "ZIP checksum sidecar does not match."
}

$extractRoot = Join-Path $VerifyRoot "unpacked"
if (Test-Path -LiteralPath $extractRoot) {
    Remove-Item -LiteralPath $extractRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $extractRoot | Out-Null
Expand-Archive -LiteralPath $zipPath -DestinationPath $extractRoot
$top = @(Get-ChildItem -LiteralPath $extractRoot -Directory)
if ($top.Count -ne 1) {
    throw "ZIP must contain exactly one top-level package directory."
}
$expandedPackage = $top[0].FullName
$manifest = Get-Content -Raw -LiteralPath (Join-Path $expandedPackage "PACKAGE_MANIFEST.json") | ConvertFrom-Json
if ($manifest.version -ne $ExpectedVersion -or $manifest.program_id -ne $ProgramId -or $manifest.exlaunch_commit -ne $ExpectedCommit) {
    throw "Expanded package manifest failed identity checks."
}

$checksumLines = @(Get-Content -LiteralPath (Join-Path $expandedPackage "CHECKSUMS.txt"))
foreach ($line in $checksumLines) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    if ($line -notmatch '^([0-9A-Fa-f]{64}) \*(.+)$') {
        throw "Malformed package checksum: $line"
    }
    $targetFile = Join-Path $expandedPackage $Matches[2].Replace('/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $targetFile -PathType Leaf)) {
        throw "Missing checksummed file: $targetFile"
    }
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $targetFile).Hash -ne $Matches[1].ToUpperInvariant()) {
        throw "Checksum mismatch for $($Matches[2])."
    }
}

$report = [ordered]@{
    schema_version = 1
    status = "PASS"
    version = $ExpectedVersion
    program_id = $ProgramId
    subsdk9_sha256 = $reportedSubsdk[0].sha256
    npdm_sha256 = $reportedNpdm[0].sha256
    zip_sha256 = $packageReport.zip.sha256
    files_verified = $checksumLines.Count
}
New-Item -ItemType Directory -Force -Path $VerifyRoot | Out-Null
[IO.File]::WriteAllText($ReportPath, (($report | ConvertTo-Json -Depth 6) + "`n"), $Utf8NoBom)
Write-Host "Verification PASS: $($checksumLines.Count) files verified, zip=$($packageReport.zip.sha256)"

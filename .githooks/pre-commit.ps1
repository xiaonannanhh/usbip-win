$ErrorActionPreference = "Stop"

if ($env:USBRELAY_SYNC_AMENDING -eq "1") {
    exit 0
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$staged = @(& git -C $repoRoot diff --cached --name-only --diff-filter=ACDMRT)
if ($LASTEXITCODE -ne 0) {
    throw "Could not inspect staged changes."
}
if ($staged.Count -eq 0) {
    exit 0
}

$versionPath = Join-Path $repoRoot "VERSION"
$changelogPath = Join-Path $repoRoot "CHANGELOG.md"
$version = (Get-Content -LiteralPath $versionPath -Raw).Trim()
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw "VERSION must use MAJOR.MINOR.PATCH format; found '$version'."
}

if ($staged -notcontains "VERSION") {
    $version = "{0}.{1}.{2}" -f $Matches[1], $Matches[2], ([int]$Matches[3] + 1)
    [System.IO.File]::WriteAllText($versionPath, "$version`n", [System.Text.Encoding]::ASCII)
}

$changelog = Get-Content -LiteralPath $changelogPath -Encoding UTF8 -Raw
if (-not $changelog.Contains("## $version - ")) {
    $date = Get-Date -Format "yyyy-MM-dd"
    $summaryLabel = -join @([char]0x63d0, [char]0x4ea4, [char]0x8bf4, [char]0x660e)
    $filesLabel = -join @([char]0x6d89, [char]0x53ca, [char]0x6587, [char]0x4ef6)
    $fullwidthColon = [char]0xff1a
    $displayFiles = @($staged | Select-Object -First 20 | ForEach-Object { "``$_``" })
    $displayFiles += "VERSION", "CHANGELOG.md"
    $displayFiles = @($displayFiles | Select-Object -Unique | Select-Object -First 20)
    $filesText = $displayFiles -join ", "
    $entry = @(
        "## $version - $date",
        "",
        "- ${summaryLabel}${fullwidthColon}[USBRELAY_PENDING_SUBJECT]",
        "- ${filesLabel}${fullwidthColon}${filesText}.",
        ""
    ) -join "`n"
    $firstEntry = $changelog.IndexOf("`n## ")
    if ($firstEntry -lt 0) {
        $updated = $changelog.TrimEnd() + "`n`n$entry"
    } else {
        $updated = $changelog.Insert($firstEntry + 1, "$entry`n")
    }
    [System.IO.File]::WriteAllText($changelogPath, $updated, [System.Text.UTF8Encoding]::new($false))
}

& git -C $repoRoot add -- VERSION CHANGELOG.md
if ($LASTEXITCODE -ne 0) {
    throw "Could not stage the automatic version record."
}
Write-Host "Prepared version $version and its changelog entry."

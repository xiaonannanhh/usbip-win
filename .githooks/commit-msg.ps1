$ErrorActionPreference = "Stop"

if ($args.Count -lt 1) {
    throw "Git did not provide the commit message path."
}

$messagePath = $args[0]
$subject = Get-Content -LiteralPath $messagePath -Encoding UTF8 | Where-Object { $_.Trim() } | Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($subject)) {
    throw "The commit message must have a non-empty subject."
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$version = (Get-Content -LiteralPath (Join-Path $repoRoot "VERSION") -Raw).Trim()
$changelogPath = Join-Path $repoRoot "CHANGELOG.md"
$changelog = Get-Content -LiteralPath $changelogPath -Encoding UTF8 -Raw
$pending = "[USBRELAY_PENDING_SUBJECT]"
$summaryLabel = -join @([char]0x63d0, [char]0x4ea4, [char]0x8bf4, [char]0x660e)
$fullwidthColon = [char]0xff1a

if ($changelog.Contains("## $version - ") -and $changelog.Contains($pending)) {
    $escapedSubject = $subject.Trim().Replace("`r", " ").Replace("`n", " ")
    $updated = $changelog.Replace("- ${summaryLabel}${fullwidthColon}$pending", "- ${summaryLabel}${fullwidthColon}$escapedSubject")
    [System.IO.File]::WriteAllText($changelogPath, $updated, [System.Text.UTF8Encoding]::new($false))
    & git -C $repoRoot add -- CHANGELOG.md
    if ($LASTEXITCODE -ne 0) {
        throw "Could not stage the completed changelog entry."
    }
}

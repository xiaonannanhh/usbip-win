$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\")).Path
if ($env:USBRELAY_SYNC_AMENDING -eq "1") {
    exit 0
}

$versionLines = @(& git -C $repoRoot show "HEAD:VERSION")
if ($LASTEXITCODE -ne 0 -or $versionLines.Count -eq 0) {
    throw "Could not read VERSION from the new commit."
}
$version = $versionLines[0].Trim()
$subjectLines = @(& git -C $repoRoot show -s --format=%s HEAD)
if ($LASTEXITCODE -ne 0 -or $subjectLines.Count -eq 0) {
    throw "Could not read the new commit subject."
}
$subject = $subjectLines[0].Trim().Replace("`r", " ").Replace("`n", " ")
$committedChangelog = @(& git -C $repoRoot show "HEAD:CHANGELOG.md") -join "`n"
if ($LASTEXITCODE -ne 0) {
    throw "Could not read CHANGELOG.md from the new commit."
}

$pending = "[USBRELAY_PENDING_SUBJECT]"
$sectionHeader = "## $version - "
$sectionStart = $committedChangelog.IndexOf($sectionHeader, [System.StringComparison]::Ordinal)
if ($sectionStart -ge 0) {
    $nextSection = $committedChangelog.IndexOf("`n## ", $sectionStart + $sectionHeader.Length, [System.StringComparison]::Ordinal)
    if ($nextSection -lt 0) {
        $nextSection = $committedChangelog.Length
    }
    $section = $committedChangelog.Substring($sectionStart, $nextSection - $sectionStart)
    if ($section.Contains($pending)) {
        $summaryLabel = -join @([char]0x63d0, [char]0x4ea4, [char]0x8bf4, [char]0x660e)
        $fullwidthColon = [char]0xff1a
        $oldLine = "- ${summaryLabel}${fullwidthColon}$pending"
        if (-not $section.Contains($oldLine)) {
            throw "The pending changelog subject is not in the expected format."
        }

        $newLine = "- ${summaryLabel}${fullwidthColon}$subject"
        $updatedSection = $section.Replace($oldLine, $newLine)
        $updatedChangelog = $committedChangelog.Substring(0, $sectionStart) + $updatedSection + $committedChangelog.Substring($nextSection)
        $changelogPath = Join-Path $repoRoot "CHANGELOG.md"
        [System.IO.File]::WriteAllText($changelogPath, $updatedChangelog, [System.Text.UTF8Encoding]::new($false))
        & git -C $repoRoot add -- CHANGELOG.md
        if ($LASTEXITCODE -ne 0) {
            throw "Could not stage the completed changelog entry for amendment."
        }

        Write-Host "Completing the committed version record and amending the commit..."
        $previousAmending = $env:USBRELAY_SYNC_AMENDING
        try {
            $env:USBRELAY_SYNC_AMENDING = "1"
            & git -C $repoRoot commit --amend --no-edit --no-verify
            if ($LASTEXITCODE -ne 0) {
                throw "Could not amend the commit with its completed changelog entry."
            }
        } finally {
            if ($null -eq $previousAmending) {
                Remove-Item Env:\USBRELAY_SYNC_AMENDING -ErrorAction SilentlyContinue
            } else {
                $env:USBRELAY_SYNC_AMENDING = $previousAmending
            }
        }

        $verifiedChangelog = @(& git -C $repoRoot show "HEAD:CHANGELOG.md") -join "`n"
        if ($LASTEXITCODE -ne 0 -or $verifiedChangelog.Contains($pending)) {
            throw "The amended commit still contains a pending changelog subject."
        }
        $verifiedVersion = @(& git -C $repoRoot show "HEAD:VERSION")
        if ($LASTEXITCODE -ne 0 -or $verifiedVersion.Count -eq 0 -or $verifiedVersion[0].Trim() -ne $version) {
            throw "The version changed unexpectedly while completing the changelog."
        }
    }
}

$upstream = @(& git -C $repoRoot rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>$null)
if ($LASTEXITCODE -ne 0 -or $upstream.Count -eq 0) {
    Write-Warning "Commit created locally, but this branch has no GitHub upstream; push was skipped."
    exit 0
}

Write-Host "Pushing committed version record to $($upstream[0])..."
& git -C $repoRoot push
if ($LASTEXITCODE -ne 0) {
    Write-Warning "Commit is local; automatic GitHub push failed. Run 'git push' after resolving the reported error."
    exit 0
}

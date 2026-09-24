$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$hookDirectory = Join-Path $repoRoot ".githooks"
foreach ($hook in @("pre-commit", "commit-msg", "post-commit")) {
    if (-not (Test-Path -LiteralPath (Join-Path $hookDirectory $hook) -PathType Leaf)) {
        throw "Git hook not found: $hook"
    }
}

foreach ($script in @("pre-commit.ps1", "commit-msg.ps1", "post-commit.ps1")) {
    if (-not (Test-Path -LiteralPath (Join-Path $hookDirectory $script) -PathType Leaf)) {
        throw "Git hook script not found: $script"
    }
}

& git -C $repoRoot config --local core.hooksPath .githooks
if ($LASTEXITCODE -ne 0) {
    throw "Could not configure this repository's Git hooks."
}

$hooksPath = @(& git -C $repoRoot config --local --get core.hooksPath)
if ($LASTEXITCODE -ne 0 -or $hooksPath[0] -ne ".githooks") {
    throw "Git hooks configuration did not persist as expected."
}

$upstream = @(& git -C $repoRoot rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>$null)
if ($LASTEXITCODE -ne 0 -or $upstream.Count -eq 0) {
    Write-Warning "Hooks are installed, but this branch has no upstream. Set one with git push -u <remote> <branch>."
} else {
    Write-Host "Hooks installed. Each commit records its version and changelog entry, then pushes to $($upstream[0])."
}

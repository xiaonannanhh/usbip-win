$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
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
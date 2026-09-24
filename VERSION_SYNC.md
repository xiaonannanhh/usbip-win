# GitHub Sync and Version Records

This repository records releases at the Git commit boundary. Install the repository hooks with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/install-git-sync.ps1
```

For each commit with staged changes, the pre-commit hook increments the patch component of `VERSION` and adds a `CHANGELOG.md` entry with the staged file list. The commit-msg hook fills in the final commit subject and stages the completed entry before Git creates the commit. After the commit succeeds, the post-commit hook pushes it to the current branch's configured upstream.

Only staged and committed changes are recorded or pushed. Saving a file does not create a commit or upload it. If network access, authentication, or a remote conflict prevents the push, the local commit is retained; resolve the issue and run `git push`.

To set an intentional version manually, stage both `VERSION` and `CHANGELOG.md` before committing; automatic patch increment is skipped for that commit.

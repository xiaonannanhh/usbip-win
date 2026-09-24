function Write-BuildVersionHeader {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$BuildDirectory
    )

    $versionPath = Join-Path $SourceRoot "VERSION"
    $version = (Get-Content -LiteralPath $versionPath -Raw).Trim()
    if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
        throw "VERSION must use MAJOR.MINOR.PATCH format; found '$version'."
    }

    $numericVersion = "{0},{1},{2},0" -f $Matches[1], $Matches[2], $Matches[3]
    $header = @(
        "#define PRODUCT_VERSION_COMMA $numericVersion",
        "#define PRODUCT_VERSION_STRING `"$version`"",
        ""
    ) -join "`n"
    [System.IO.File]::WriteAllText(
        (Join-Path $BuildDirectory "version_resource.h"),
        $header,
        [System.Text.Encoding]::ASCII)
}

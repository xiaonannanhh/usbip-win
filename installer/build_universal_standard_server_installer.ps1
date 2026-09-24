[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$OutputDir,
    [string]$ClPath = "C:\BuildTools2019\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x86\cl.exe",
    [string]$RcPath = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x86\rc.exe"
)

$ErrorActionPreference = "Stop"

$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = (Resolve-Path (Join-Path $ScriptDirectory "..")).Path
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = [System.IO.Path]::GetFullPath(
        (Join-Path $ScriptDirectory "..\..\..\..\..\outputs"))
}

$InstallerDir = Join-Path $SourceRoot "installer"
. (Join-Path $InstallerDir "version.ps1")
$LauncherSource = Join-Path $InstallerDir "universal_launcher.c"
$ResourceSource = Join-Path $InstallerDir "universal.rc"
$ResourceHeader = Join-Path $InstallerDir "universal_resource.h"
$IconPath = Join-Path $InstallerDir "app.ico"
$ManifestPath = Join-Path $InstallerDir "setup.manifest"
$BuildDir = Join-Path $env:TEMP ("usbrelay-universal-" + [guid]::NewGuid().ToString("N"))
$VcRoot = [System.IO.Path]::GetFullPath(
    (Join-Path (Split-Path $ClPath -Parent) "..\..\.."))
$SdkVersion = Split-Path (Split-Path (Split-Path $RcPath -Parent) -Parent) -Leaf
$SdkRoot = [System.IO.Path]::GetFullPath(
    (Join-Path (Split-Path $RcPath -Parent) "..\..\.."))
$SdkInclude = Join-Path $SdkRoot "Include\$SdkVersion"
$SdkLib = Join-Path $SdkRoot "Lib\$SdkVersion"

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [string]$WorkingDirectory
    )

    if ($WorkingDirectory) {
        Push-Location $WorkingDirectory
        try {
            & $FilePath @Arguments
        } finally {
            Pop-Location
        }
    } else {
        & $FilePath @Arguments
    }

    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE."
    }
}

foreach ($tool in @($ClPath, $RcPath)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Build tool not found: $tool"
    }
}
foreach ($file in @(
    $LauncherSource,
    $ResourceSource,
    $ResourceHeader,
    $IconPath,
    $ManifestPath)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Universal installer source not found: $file"
    }
}
foreach ($directory in @(
    (Join-Path $VcRoot "include"),
    (Join-Path $VcRoot "lib\x86"),
    (Join-Path $SdkInclude "ucrt"),
    (Join-Path $SdkInclude "shared"),
    (Join-Path $SdkInclude "um"),
    (Join-Path $SdkLib "ucrt\x86"),
    (Join-Path $SdkLib "um\x86"))) {
    if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
        throw "Build include or library directory not found: $directory"
    }
}

$env:INCLUDE = @(
    (Join-Path $SdkInclude "ucrt"),
    (Join-Path $SdkInclude "shared"),
    (Join-Path $SdkInclude "um"),
    (Join-Path $VcRoot "include")) -join ";"
$env:LIB = @(
    (Join-Path $VcRoot "lib\x86"),
    (Join-Path $SdkLib "ucrt\x86"),
    (Join-Path $SdkLib "um\x86")) -join ";"

New-Item -ItemType Directory -Path $BuildDir | Out-Null
Write-BuildVersionHeader -SourceRoot $SourceRoot -BuildDirectory $BuildDir
Copy-Item -LiteralPath $LauncherSource -Destination $BuildDir -Force
Copy-Item -LiteralPath $ResourceSource -Destination $BuildDir -Force
Copy-Item -LiteralPath $ResourceHeader -Destination $BuildDir -Force
Copy-Item -LiteralPath $IconPath -Destination $BuildDir -Force
Copy-Item -LiteralPath $ManifestPath -Destination (Join-Path $BuildDir "setup.manifest") -Force

$compileFlags = @(
    "/nologo",
    "/c",
    "/O2",
    "/W4",
    "/MT",
    "/GS",
    "/utf-8",
    "/D_WIN32_WINNT=0x0601",
    "/DWINVER=0x0601",
    "/DUNICODE",
    "/D_UNICODE",
    "/DWIN32_LEAN_AND_MEAN",
    "/I$BuildDir"
)

try {
    foreach ($kind in @("standard-server")) {
        $x86Name = "USBRelay-Standard-Server-Setup-x86.exe"
        $x64Name = "USBRelay-Standard-Server-Setup-x64.exe"
        $x86Path = Join-Path $OutputDir $x86Name
        $x64Path = Join-Path $OutputDir $x64Name

        if (-not (Test-Path -LiteralPath $x86Path -PathType Leaf)) {
            throw "Architecture-specific installer not found: $x86Path"
        }
        if (-not (Test-Path -LiteralPath $x64Path -PathType Leaf)) {
            throw "Architecture-specific installer not found: $x64Path"
        }

        Copy-Item -LiteralPath $x86Path `
            -Destination (Join-Path $BuildDir "installer-x86.exe") -Force
        Copy-Item -LiteralPath $x64Path `
            -Destination (Join-Path $BuildDir "installer-x64.exe") -Force

        $resourceFile = Join-Path $BuildDir "$kind-universal.res"
        $objectFile = Join-Path $BuildDir "$kind-universal.obj"
        $resourceDefine = "UNIVERSAL_STANDARD_SERVER"
        $outputName = "USBRelay-Standard-Server-Setup-Universal.exe"
        $outputPath = Join-Path $OutputDir $outputName
        $clientDefine = "/DUNIVERSAL_STANDARD_SERVER"

        Invoke-Native -FilePath $ClPath -Arguments (
            $compileFlags + @(
                $clientDefine,
                "/Fo$objectFile",
                (Join-Path $BuildDir "universal_launcher.c")))

        Invoke-Native -FilePath $RcPath -WorkingDirectory $BuildDir -Arguments @(
            "/nologo",
            "/d$resourceDefine",
            "/I$BuildDir",
            "/fo$resourceFile",
            (Join-Path $BuildDir "universal.rc")
        )

        Invoke-Native -FilePath $ClPath -Arguments @(
            "/nologo",
            $objectFile,
            $resourceFile,
            "/Fe$outputPath",
            "/link",
            "/SUBSYSTEM:CONSOLE,6.01",
            "/MANIFEST:NO",
            "/DYNAMICBASE",
            "/NXCOMPAT",
            "/OPT:REF",
            "/OPT:ICF",
            "user32.lib",
            "shell32.lib"
        )

        if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
            throw "The compiler did not create $outputPath."
        }
    }
} finally {
    if (Test-Path -LiteralPath $BuildDir -PathType Container) {
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }
}

Write-Host "Created:"
Write-Host (Join-Path $OutputDir "USBRelay-Standard-Server-Setup-Universal.exe")

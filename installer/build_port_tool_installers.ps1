[CmdletBinding()]
param(
    [string]$SourceRoot,
    [string]$OutputDir,
    [ValidateSet("x86", "x64")]
    [string]$Architecture = "x86",
    [string]$ClPath,
    [string]$RcPath,
    [string]$MakeCabPath = "$env:SystemRoot\System32\makecab.exe"
)

$ErrorActionPreference = "Stop"

$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $ScriptDirectory ".."))
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = [System.IO.Path]::GetFullPath(
        (Join-Path $ScriptDirectory "..\..\..\..\..\outputs"))
}

if (-not $ClPath) {
    $ClPath = if ($Architecture -eq "x64") {
        "C:\BuildTools2019\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x64\cl.exe"
    } else {
        "C:\BuildTools2019\VC\Tools\MSVC\14.29.30133\bin\Hostx64\x86\cl.exe"
    }
}
if (-not $RcPath) {
    $RcPath = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\$Architecture\rc.exe"
}

$InstallerDir = Join-Path $SourceRoot "installer"
. (Join-Path $InstallerDir "version.ps1")
$IconPath = Join-Path $InstallerDir "app.ico"
$LinkPath = Join-Path (Split-Path $ClPath -Parent) "link.exe"
$BuildDir = Join-Path $env:TEMP ("usbrelay-port-tool-" + [guid]::NewGuid().ToString("N"))
$ToolDir = Join-Path $OutputDir "USBRelay-Standard-Port-Tool-$Architecture"
$VcRoot = [System.IO.Path]::GetFullPath(
    (Join-Path (Split-Path $ClPath -Parent) "..\..\.."))
$SdkVersion = Split-Path (Split-Path (Split-Path $RcPath -Parent) -Parent) -Leaf
$SdkRoot = [System.IO.Path]::GetFullPath(
    (Join-Path (Split-Path $RcPath -Parent) "..\..\.."))
$SdkInclude = Join-Path $SdkRoot "Include\$SdkVersion"
$SdkLib = Join-Path $SdkRoot "Lib\$SdkVersion"
$ArchitectureDefine = if ($Architecture -eq "x64") { "/DUI_X64" } else { "/DUI_X86" }
$SetupArchitectureDefine = if ($Architecture -eq "x64") { "/DSETUP_X64" } else { "/DSETUP_X86" }
$UiManifestName = if ($Architecture -eq "x64") { "ui.x64.manifest" } else { "ui.manifest" }
$SetupManifestName = if ($Architecture -eq "x64") { "setup.x64.manifest" } else { "setup.manifest" }

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE."
    }
}

function New-PayloadCab {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$PayloadDir,
        [Parameter(Mandatory = $true)][string[]]$Files
    )

    $ddfPath = Join-Path $BuildDir "$Name.ddf"
    $cabPath = Join-Path $BuildDir "$Name.cab"
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add(".OPTION EXPLICIT")
    $lines.Add(".Set CabinetNameTemplate=$Name.cab")
    $lines.Add(".Set DiskDirectoryTemplate=.")
    $lines.Add(".Set CompressionType=MSZIP")
    $lines.Add(".Set Cabinet=on")
    $lines.Add(".Set Compress=on")
    $lines.Add(".Set UniqueFiles=off")
    $lines.Add(".Set RptFileName=nul")
    $lines.Add(".Set InfFileName=nul")
    $lines.Add(".Set MaxDiskSize=0")

    foreach ($file in $Files) {
        $payloadFile = Join-Path $PayloadDir $file
        if (-not (Test-Path -LiteralPath $payloadFile -PathType Leaf)) {
            throw "Payload file not found: $payloadFile"
        }
        $lines.Add('"' + $payloadFile + '" "' + $file + '"')
    }

    [System.IO.File]::WriteAllLines($ddfPath, $lines,
        [System.Text.Encoding]::ASCII)
    Push-Location $BuildDir
    try {
        $null = Invoke-Native -FilePath $MakeCabPath -Arguments @("/F", $ddfPath)
    } finally {
        Pop-Location
    }
    if (-not (Test-Path -LiteralPath $cabPath -PathType Leaf)) {
        throw "makecab did not create $cabPath."
    }
    return $cabPath
}

function New-UiExecutable {
    param(
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [Parameter(Mandatory = $true)][string]$OutputName
    )

    $resourceDefine = "UI_PORT_TOOL"
    $resourceSource = Join-Path $BuildDir "$Kind-ui-resources.rc"
    $manifestSource = Join-Path $BuildDir "$Kind-ui.manifest"
    $resourceFile = Join-Path $BuildDir "$Kind-ui.res"
    $commonObject = Join-Path $BuildDir "$Kind-ui-common.obj"
    $uiObject = Join-Path $BuildDir "$Kind-ui.obj"
    $outputPath = Join-Path $OutputDirectory $OutputName
    $compileFlags = @(
        "/nologo", "/c", "/O2", "/W4", "/MT", "/GS", "/utf-8",
        "/D_WIN32_WINNT=0x0601", "/DWINVER=0x0601", "/DUNICODE", "/D_UNICODE",
        "/DWIN32_LEAN_AND_MEAN"
    )

    Copy-Item -LiteralPath (Join-Path $InstallerDir "ui_resources.rc") -Destination $resourceSource -Force
    Copy-Item -LiteralPath (Join-Path $InstallerDir "ui_resource.h") -Destination $BuildDir -Force
    Copy-Item -LiteralPath (Join-Path $InstallerDir $UiManifestName) -Destination $manifestSource -Force
    Copy-Item -LiteralPath $manifestSource -Destination (Join-Path $BuildDir "ui.manifest") -Force

    Invoke-Native -FilePath $RcPath -Arguments @(
        "/nologo", "/d$resourceDefine", $ArchitectureDefine,
        "/fo$resourceFile", $resourceSource
    )
    Invoke-Native -FilePath $ClPath -Arguments (
        $compileFlags + @("/Fo$commonObject", (Join-Path $InstallerDir "ui_common.c")))
    Invoke-Native -FilePath $ClPath -Arguments (
        $compileFlags + @("/D$resourceDefine", $ArchitectureDefine,
            "/Fo$uiObject", (Join-Path $InstallerDir "usbrelay_port_tool_ui.c")))
    Invoke-Native -FilePath $ClPath -Arguments @(
        "/nologo", "/O2", "/MT", "/GS",
        $commonObject, $uiObject, $resourceFile, "/Fe$outputPath",
        "/link", "/SUBSYSTEM:WINDOWS,6.01", "/MANIFEST:NO", "/DYNAMICBASE",
        "/NXCOMPAT", "/OPT:REF", "/OPT:ICF",
        "comctl32.lib", "user32.lib", "gdi32.lib", "ws2_32.lib",
        "shell32.lib", "advapi32.lib", "ole32.lib", "uuid.lib",
        "winspool.lib", "iphlpapi.lib"
    )
    if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
        throw "The compiler did not create $outputPath."
    }
}

function Copy-PayloadFiles {
    $destination = $ToolDir
    $generatedFiles = @("usbrelay-port-tool-ui.exe")
    $supportFiles = @(
        "install-port-tool.cmd",
        "uninstall-port-tool.cmd",
        "README_PORT_TOOL_CN.txt",
        "README_PORT_TOOL_MANUAL_CN.md"
    )

    foreach ($file in $generatedFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $destination $file) -PathType Leaf)) {
            throw "Generated payload file not found: $file"
        }
    }
    foreach ($file in $supportFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $InstallerDir $file) -PathType Leaf)) {
            throw "Payload source file not found: $file"
        }
        Copy-Item -LiteralPath (Join-Path $InstallerDir $file) -Destination $destination -Force
    }
    Copy-Item -LiteralPath (Join-Path $SourceRoot "LICENSE") -Destination (Join-Path $destination "LICENSE") -Force
    Copy-Item -LiteralPath (Join-Path $SourceRoot "LICENSE") -Destination (Join-Path $destination "COPYING") -Force
}

function New-SetupExe {
    param(
        [Parameter(Mandatory = $true)][string]$Kind,
        [Parameter(Mandatory = $true)][string]$PayloadCab,
        [Parameter(Mandatory = $true)][string]$OutputName
    )

    $resourceFile = Join-Path $BuildDir "$Kind.res"
    $objectFile = Join-Path $BuildDir "$Kind.obj"
    $outputPath = Join-Path $OutputDir $OutputName
    $define = "SETUP_PORT_TOOL"

    Copy-Item -LiteralPath $PayloadCab -Destination (Join-Path $BuildDir "setup-payload.cab") -Force
    Copy-Item -LiteralPath (Join-Path $InstallerDir "setup.rc") -Destination $BuildDir -Force
    Copy-Item -LiteralPath (Join-Path $InstallerDir $SetupManifestName) -Destination (Join-Path $BuildDir "setup.manifest") -Force
    Copy-Item -LiteralPath (Join-Path $InstallerDir "resource.h") -Destination $BuildDir -Force

    Invoke-Native -FilePath $RcPath -Arguments @(
        "/nologo", "/d$define", $SetupArchitectureDefine,
        "/fo$resourceFile", (Join-Path $BuildDir "setup.rc")
    )
    Invoke-Native -FilePath $ClPath -Arguments @(
        "/nologo", "/O2", "/W4", "/MT", "/GS", "/utf-8",
        "/D_WIN32_WINNT=0x0601", "/DWINVER=0x0601", "/DUNICODE", "/D_UNICODE",
        "/DWIN32_LEAN_AND_MEAN", "/D$define", $SetupArchitectureDefine,
        "/Fo$objectFile", (Join-Path $InstallerDir "setup.c"), "/Fe$outputPath",
        "/link", $resourceFile, "/SUBSYSTEM:CONSOLE,6.01", "/MANIFEST:NO",
        "/DYNAMICBASE", "/NXCOMPAT", "/OPT:REF", "/OPT:ICF"
    )
    if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
        throw "The compiler did not create $outputPath."
    }
}

foreach ($tool in @($ClPath, $RcPath, $MakeCabPath, $LinkPath)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Build tool not found: $tool"
    }
}
foreach ($file in @(
    (Join-Path $SourceRoot "LICENSE"),
    $IconPath,
    (Join-Path $InstallerDir "usbrelay_port_tool_ui.c"),
    (Join-Path $InstallerDir "install-port-tool.cmd"),
    (Join-Path $InstallerDir "uninstall-port-tool.cmd"),
    (Join-Path $InstallerDir "README_PORT_TOOL_CN.txt"),
    (Join-Path $InstallerDir "README_PORT_TOOL_MANUAL_CN.md"))) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Source file not found: $file"
    }
}
foreach ($directory in @(
    (Join-Path $VcRoot "include"),
    (Join-Path $VcRoot "lib\$Architecture"),
    (Join-Path $SdkInclude "ucrt"),
    (Join-Path $SdkInclude "shared"),
    (Join-Path $SdkInclude "um"),
    (Join-Path $SdkLib "ucrt\$Architecture"),
    (Join-Path $SdkLib "um\$Architecture"))) {
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
    (Join-Path $VcRoot "lib\$Architecture"),
    (Join-Path $SdkLib "ucrt\$Architecture"),
    (Join-Path $SdkLib "um\$Architecture")) -join ";"

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
Write-BuildVersionHeader -SourceRoot $SourceRoot -BuildDirectory $BuildDir
if (Test-Path -LiteralPath $ToolDir -PathType Container) {
    Write-Host "Updating generated files in $ToolDir"
}
New-Item -ItemType Directory -Path $ToolDir -Force | Out-Null
Copy-Item -LiteralPath $IconPath -Destination (Join-Path $BuildDir "app.ico") -Force

try {
    New-UiExecutable -Kind "port-tool" `
        -OutputDirectory $ToolDir `
        -OutputName "usbrelay-port-tool-ui.exe"
    Copy-PayloadFiles
    $payloadFiles = @(
        "usbrelay-port-tool-ui.exe",
        "install-port-tool.cmd",
        "uninstall-port-tool.cmd",
        "README_PORT_TOOL_CN.txt",
        "README_PORT_TOOL_MANUAL_CN.md",
        "LICENSE",
        "COPYING"
    )
    $payloadCab = New-PayloadCab -Name "port-tool-payload" `
        -PayloadDir $ToolDir -Files $payloadFiles
    New-SetupExe -Kind "port-tool" -PayloadCab $payloadCab `
        -OutputName "USBRelay-Standard-Port-Tool-Setup-$Architecture.exe"
} finally {
    if (Test-Path -LiteralPath $BuildDir -PathType Container) {
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }
}

Write-Host "Created:"
Write-Host (Join-Path $OutputDir "USBRelay-Standard-Port-Tool-Setup-$Architecture.exe")
Write-Host $ToolDir

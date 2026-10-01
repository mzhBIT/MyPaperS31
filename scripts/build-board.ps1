param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('MyPaperP4', 'MyPaperS31')]
    [string]$Board,
    [ValidateSet('build', 'reconfigure')]
    [string]$Action = 'build'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $env:IDF_PATH) { throw 'Activate the matching ESP-IDF environment first.' }
$versionFile = Join-Path $env:IDF_PATH 'tools/cmake/version.cmake'
$versionText = Get-Content -LiteralPath $versionFile -Raw
$major = [regex]::Match($versionText, 'IDF_VERSION_MAJOR\s+(\d+)').Groups[1].Value
$minor = [regex]::Match($versionText, 'IDF_VERSION_MINOR\s+(\d+)').Groups[1].Value
$patch = [regex]::Match($versionText, 'IDF_VERSION_PATCH\s+(\d+)').Groups[1].Value
if ($Board -eq 'MyPaperP4') {
    if ("$major.$minor.$patch" -ne '5.3.5') { throw 'MyPaperP4 requires ESP-IDF 5.3.5.' }
    $target = 'esp32p4'
    $buildDir = 'build-p4'
    $defaults = 'sdkconfig.defaults;configs/mypaperp4/sdkconfig.defaults'
} else {
    if ("$major.$minor" -ne '6.1') { throw 'MyPaperS31 requires ESP-IDF 6.1.' }
    $target = 'esp32s31'
    $buildDir = 'build-s31'
    $defaults = 'configs/mypapers31/sdkconfig.defaults'
}
$configPath = Join-Path $projectRoot "$buildDir/sdkconfig"
$idfScript = Join-Path $env:IDF_PATH 'tools/idf.py'
Push-Location $projectRoot
try {
    & python $idfScript -B $buildDir "-DMYPAPER_BOARD=$Board" "-DIDF_TARGET=$target" `
        "-DSDKCONFIG=$configPath" "-DSDKCONFIG_DEFAULTS=$defaults" $Action
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $Board (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}

param(
    [ValidateSet('build', 'flash', 'monitor', 'menuconfig')]
    [string]$Action = 'build',
    [ValidateSet('Baseline', 'PhyTrack', 'FullCal')]
    [string]$Variant = 'Baseline',
    [string]$Port = 'COM5',
    [int]$Baud = 921600,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$testRoot = Join-Path $projectRoot 'tools/s31-iperf'
$buildName = switch ($Variant) {
    'PhyTrack' { 'build-phy-track' }
    'FullCal' { 'build-full-cal' }
    default { 'build' }
}
$buildRoot = Join-Path $testRoot $buildName
# Flash/monitor existing artifacts without an activated IDF shell or rebuilding.
if ($Action -in @('flash', 'monitor')) {
    $cachePath = Join-Path $buildRoot 'CMakeCache.txt'
    $manifestPath = Join-Path $buildRoot 'flasher_args.json'
    if (-not (Test-Path -LiteralPath $cachePath) -or -not (Test-Path -LiteralPath $manifestPath)) {
        throw 'Build the S31 iperf project first; build metadata is missing.'
    }
    $pythonEntry = [regex]::Match([System.IO.File]::ReadAllText($cachePath), '(?m)^PYTHON:[^=\r\n]+=([^\r\n]+)')
    if (-not $pythonEntry.Success -or -not (Test-Path -LiteralPath $pythonEntry.Groups[1].Value)) {
        throw 'The build Python environment is missing. Activate IDF 6.1 and rebuild the test project.'
    }
    $testPython = $pythonEntry.Groups[1].Value
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.extra_esptool_args.chip -ne 'esp32s31') { throw 'Unexpected flash target; expected esp32s31.' }
    if ($Action -eq 'flash') {
        $toolArgs = @('-m', 'esptool', '--chip', 'esp32s31', '--port', $Port, '--baud', "$Baud",
                      '--before', $manifest.extra_esptool_args.before, '--after', $manifest.extra_esptool_args.after)
        if (-not $manifest.extra_esptool_args.stub) { $toolArgs += '--no-stub' }
        $toolArgs += @('write-flash') + @($manifest.write_flash_args)
        foreach ($entry in $manifest.flash_files.PSObject.Properties) {
            $binaryPath = Join-Path $buildRoot $entry.Value
            if (-not (Test-Path -LiteralPath $binaryPath)) { throw "Missing firmware: $binaryPath" }
            $toolArgs += @($entry.Name, $binaryPath)
        }
    } else {
        $elfPath = Join-Path $buildRoot 's31_iperf.elf'
        if (-not (Test-Path -LiteralPath $elfPath)) { throw "Missing ELF: $elfPath" }
        $toolArgs = @('-m', 'esp_idf_monitor', '--target', 'esp32s31', '-p', $Port, '-b', '115200', $elfPath)
    }
    Write-Host "Isolated S31 iperf: $Variant $Action using $testPython"
    if ($DryRun) {
        Write-Host ($testPython + ' ' + ($toolArgs -join ' '))
        return
    }
    $env:PYTHONUTF8 = '1'
    & $testPython @toolArgs
    if ($LASTEXITCODE -ne 0) { throw "S31 iperf $Action failed (exit $LASTEXITCODE)." }
    return
}
if ($DryRun) { throw '-DryRun is available for flash/monitor only.' }
if (-not $env:IDF_PATH) { throw 'Activate ESP-IDF 6.1 first.' }
$version = Get-Content -LiteralPath (Join-Path $env:IDF_PATH 'tools/cmake/version.cmake') -Raw
if ($version -notmatch 'IDF_VERSION_MAJOR\s+6' -or $version -notmatch 'IDF_VERSION_MINOR\s+1') {
    throw 'The isolated S31 iperf project requires ESP-IDF 6.1.'
}
$idfScript = Join-Path $env:IDF_PATH 'tools/idf.py'
Push-Location $testRoot
try {
    Write-Host "Isolated official S31 iperf: $Variant $Action (reader build/config unchanged)"
    $idfArgs = @('-B', $buildRoot)
    if ($Variant -ne 'Baseline') {
        $defaults = "$testRoot/sdkconfig.defaults;$testRoot/board.defaults;$testRoot/phy-tracking.defaults"
        if ($Variant -eq 'FullCal') { $defaults += ";$testRoot/full-calibration.defaults" }
        $idfArgs += @("-DSDKCONFIG=$buildRoot/sdkconfig",
                      "-DSDKCONFIG_DEFAULTS=$defaults")
    }
    & python $idfScript @idfArgs $Action
    if ($LASTEXITCODE -ne 0) { throw "S31 iperf $Action failed (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}

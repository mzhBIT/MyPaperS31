param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('MyPaperP4', 'MyPaperS31')]
    [string]$Board,
    [ValidateSet('build', 'reconfigure', 'flash', 'monitor')]
    [string]$Action = 'build',
    [ValidateSet('Current', 'Debug', 'Release')]
    [string]$Profile = 'Current',
    [string]$Port,
    [int]$Baud = 921600
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
$baseBuildDir = $buildDir
if ($Profile -ne 'Current') {
    $buildDir += '-' + $Profile.ToLowerInvariant()
}
$configPath = Join-Path $projectRoot "$buildDir/sdkconfig"
if ($Profile -ne 'Current' -and $Action -in @('build', 'reconfigure')) {
    # Keep board/user settings, but never alter the original sdkconfig.
    [System.IO.Directory]::CreateDirectory((Split-Path -Parent $configPath)) | Out-Null
    if (-not (Test-Path -LiteralPath $configPath)) {
        $seed = Join-Path $projectRoot "$baseBuildDir/sdkconfig"
        if ($Board -eq 'MyPaperP4' -and (Test-Path -LiteralPath (Join-Path $projectRoot 'sdkconfig'))) {
            $seed = Join-Path $projectRoot 'sdkconfig'
        }
        if (Test-Path -LiteralPath $seed) { Copy-Item -LiteralPath $seed -Destination $configPath }
    }
    $profilePath = Join-Path $projectRoot "configs/logging/$($Profile.ToLowerInvariant()).defaults"
    $overlay = [System.IO.File]::ReadAllLines($profilePath)
    $keys = @($overlay | ForEach-Object {
        if ($_ -match '^(?:# )?(CONFIG_[A-Z0-9_]+)(?:=| is not set)') { $Matches[1] }
    })
    $lines = @()
    if (Test-Path -LiteralPath $configPath) {
        $lines = @([System.IO.File]::ReadAllLines($configPath) | Where-Object {
            -not ($_ -match '^(?:# )?(CONFIG_[A-Z0-9_]+)(?:=| is not set)' -and $Matches[1] -in $keys)
        })
    }
    # Enforce the central profile on every invocation; defaults alone cannot
    # override values already stored by menuconfig.
    $actualValues = @{}
    if (Test-Path -LiteralPath $configPath) {
        foreach ($line in [System.IO.File]::ReadAllLines($configPath)) {
            if ($line -match '^(CONFIG_[A-Z0-9_]+)=(.*)$') { $actualValues[$Matches[1]] = $Matches[2] }
        }
    }
    $needsWrite = -not (Test-Path -LiteralPath $configPath)
    foreach ($line in $overlay) {
        if ($line -match '^(CONFIG_[A-Z0-9_]+)=(.*)$') {
            if ($actualValues[$Matches[1]] -ne $Matches[2]) { $needsWrite = $true }
        } elseif ($line -match '^# (CONFIG_[A-Z0-9_]+) is not set$') {
            if ($actualValues.ContainsKey($Matches[1]) -and $actualValues[$Matches[1]] -ne 'n') { $needsWrite = $true }
        }
    }
    if ($needsWrite) {
        [System.IO.File]::WriteAllLines($configPath, [string[]]($lines + $overlay), (New-Object System.Text.UTF8Encoding($false)))
    }
    $defaults += ";configs/logging/$($Profile.ToLowerInvariant()).defaults"
}
$idfScript = Join-Path $env:IDF_PATH 'tools/idf.py'
Push-Location $projectRoot
try {
    $portArgs = @()
    if ($Port) { $portArgs = @('-p', $Port) }
    Write-Host "Board=$Board Profile=$Profile Build=$buildDir"
    if ($Action -eq 'flash') {
        # Flash the already-built artifacts; do not invoke ninja/CMake or the
        # component registry. Rebuild explicitly when sources/config change.
        if (-not $Port) { throw 'Specify the serial port with -Port COM10.' }
        $flashDir = Join-Path $projectRoot $buildDir
        $manifestPath = Join-Path $flashDir 'flasher_args.json'
        if (-not (Test-Path -LiteralPath $manifestPath)) { throw "Build $Board $Profile first: missing $manifestPath" }
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        if ($manifest.extra_esptool_args.chip -ne $target) { throw 'Flash manifest target does not match the selected board.' }
        $flashArgs = @()
        foreach ($entry in $manifest.flash_files.PSObject.Properties) {
            $binaryPath = Join-Path $flashDir $entry.Value
            if (-not (Test-Path -LiteralPath $binaryPath)) { throw "Build first: missing $binaryPath" }
            $flashArgs += @($entry.Name, $binaryPath)
        }
        $resetArgs = $manifest.extra_esptool_args
        $writeOptions = @($manifest.write_flash_args)
        & python -m esptool --chip $target --port $Port --baud $Baud `
            --before $resetArgs.before --after $resetArgs.after write_flash @writeOptions @flashArgs
        if ($LASTEXITCODE -ne 0) { throw "Flash failed (exit $LASTEXITCODE)." }
        return
    }
    & python $idfScript -B $buildDir "-DMYPAPER_BOARD=$Board" "-DIDF_TARGET=$target" `
        "-DSDKCONFIG=$configPath" "-DSDKCONFIG_DEFAULTS=$defaults" @portArgs $Action
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $Board (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}

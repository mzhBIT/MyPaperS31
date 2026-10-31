param(
    [ValidateSet('build','reconfigure','flash','monitor')][string]$Action='build',
    [string]$Port,
    [int]$Baud=921600,
    [switch]$DryRun
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$build='build-s31-emulator'
$buildRoot=Join-Path $root $build
# Flash/monitor reuse the exact SDK and Python recorded by the existing build.
if($Action -in @('flash','monitor')){
    if(-not $Port){throw 'Specify -Port COM5.'}
    $descriptionPath=Join-Path $buildRoot 'project_description.json'
    $cachePath=Join-Path $buildRoot 'CMakeCache.txt'
    if(-not (Test-Path -LiteralPath $descriptionPath) -or -not (Test-Path -LiteralPath $cachePath)){
        throw 'Build the S31 emulator first; build metadata is missing.'
    }
    $description=Get-Content -LiteralPath $descriptionPath -Raw | ConvertFrom-Json
    if($description.project_name -ne 'MyPaperEmulatorS31' -or $description.target -ne 'esp32s31'){
        throw 'Unexpected build identity; expected MyPaperEmulatorS31 for esp32s31.'
    }
    $pythonEntry=[regex]::Match([System.IO.File]::ReadAllText($cachePath),'(?m)^PYTHON:[^=\r\n]+=([^\r\n]+)')
    if(-not $pythonEntry.Success -or -not (Test-Path -LiteralPath $pythonEntry.Groups[1].Value)){
        throw 'The build Python environment is missing. Activate IDF 6.1 and rebuild the emulator.'
    }
    $buildPython=$pythonEntry.Groups[1].Value
    $sdkVersionPath=Join-Path $description.idf_path 'tools/cmake/version.cmake'
    if(-not (Test-Path -LiteralPath $sdkVersionPath)){throw 'The build ESP-IDF installation is missing.'}
    $sdkVersion=Get-Content -LiteralPath $sdkVersionPath -Raw
    if($sdkVersion -notmatch 'IDF_VERSION_MAJOR\s+6' -or $sdkVersion -notmatch 'IDF_VERSION_MINOR\s+1'){
        throw 'S31 emulator requires ESP-IDF 6.1.'
    }
    $compilerDirectory=$null
    if($Action -eq 'flash'){
        $artifact=Join-Path $buildRoot 'MyPaperEmulatorS31.bin'
        $toolArgs=@((Join-Path $root 'tools/install_emulator.py'),'--image',$artifact,'--port',$Port,'--baud',"$Baud")
    }else{
        $artifact=Join-Path $buildRoot 'MyPaperEmulatorS31.elf'
        $toolArgs=@('-m','esp_idf_monitor','--target','esp32s31','-p',$Port,'-b','115200',$artifact)
        $compilerDirectory=Split-Path -Parent $description.c_compiler
    }
    if(-not (Test-Path -LiteralPath $artifact)){throw "Missing firmware: $artifact"}
    Write-Host "Emulator $Action using $buildPython"
    Write-Host "ESP-IDF: $($description.idf_path)"
    if($DryRun){
        Write-Host ($buildPython+' '+($toolArgs -join ' '))
        return
    }
    $previousIdf=$env:IDF_PATH
    $previousUtf8=$env:PYTHONUTF8
    $previousPath=$env:PATH
    try{
        $env:IDF_PATH=$description.idf_path
        $env:PYTHONUTF8='1'
        if($compilerDirectory){$env:PATH=$compilerDirectory+';'+$env:PATH}
        & $buildPython @toolArgs
        if($LASTEXITCODE -ne 0){throw "Emulator $Action failed (exit $LASTEXITCODE)."}
    }finally{
        $env:IDF_PATH=$previousIdf
        $env:PYTHONUTF8=$previousUtf8
        $env:PATH=$previousPath
    }
    return
}
if($DryRun){throw '-DryRun is available for flash/monitor only.'}
if(-not $env:IDF_PATH){throw 'Activate ESP-IDF 6.1 first.'}
$version=Get-Content -LiteralPath (Join-Path $env:IDF_PATH 'tools/cmake/version.cmake') -Raw
if($version -notmatch 'IDF_VERSION_MAJOR\s+6' -or $version -notmatch 'IDF_VERSION_MINOR\s+1'){
    throw 'S31 emulator requires ESP-IDF 6.1.'
}
Push-Location $root
try {
        $portArgs=@();if($Port){$portArgs=@('-p',$Port)}
        & python (Join-Path $env:IDF_PATH 'tools/idf.py') -B $build `
            '-DMYPAPER_BOARD=MyPaperS31' '-DMYPAPER_FIRMWARE=Emulator' '-DIDF_TARGET=esp32s31' `
            "-DSDKCONFIG=$root/$build/sdkconfig" '-DSDKCONFIG_DEFAULTS=firmware/emulator/sdkconfig.defaults' `
            @portArgs $Action
    if($LASTEXITCODE -ne 0){throw "Emulator $Action failed (exit $LASTEXITCODE)."}
} finally {Pop-Location}

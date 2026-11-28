param(
    [string]$DownloaderUrl,
    [int]$Port = 8091
)

$ErrorActionPreference = 'Stop'
$venv = Join-Path $PSScriptRoot '.venv'
$python = Join-Path $venv 'Scripts/python.exe'
if (-not (Test-Path -LiteralPath $python)) {
    python -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw 'Could not create the Python environment' }
}

$requirements = 'requirements.txt'
if ($DownloaderUrl) { $requirements = 'requirements-live.txt' }
$requirementsPath = Join-Path $PSScriptRoot $requirements
$fingerprint = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'requirements.txt')).Hash
if ($DownloaderUrl) { $fingerprint += (Get-FileHash -LiteralPath $requirementsPath).Hash }
$installed = Join-Path $venv ($requirements + '.installed')
if (-not (Test-Path -LiteralPath $installed) -or (Get-Content -LiteralPath $installed -Raw).Trim() -ne $fingerprint) {
    & $python -m pip install -r $requirementsPath
    if ($LASTEXITCODE -ne 0) { throw 'Could not install the service dependencies' }
    Set-Content -LiteralPath $installed -Value $fingerprint -Encoding ascii
}

$previousToken = $env:XHS_API_TOKEN
$serviceArgs = @('--mode', 'demo', '--port', $Port, '--host', '127.0.0.1')
if ($DownloaderUrl) {
    if (-not $env:XHS_API_TOKEN) {
        $env:XHS_API_TOKEN = (& $python -c 'import secrets; print(secrets.token_urlsafe(24))').Trim()
    }
    Write-Host "Reader token: $env:XHS_API_TOKEN"
    $serviceArgs = @('--mode', 'live', '--downloader-url', $DownloaderUrl, '--port', $Port, '--host', '127.0.0.1')
}

Push-Location (Join-Path $PSScriptRoot '../..')
try {
    Write-Host "Test page: http://127.0.0.1:$Port/"
    & $python -m tools.xhs_reader_server @serviceArgs
    if ($LASTEXITCODE -ne 0) { throw 'Reader server exited with an error' }
} finally {
    $env:XHS_API_TOKEN = $previousToken
    Pop-Location
}

param(
    [ValidateSet('v2', 'v1', 'cyd', 'all')][string]$Board = 'v2',
    [string]$IdfPath = $env:IDF_PATH,
    [string]$IdfProfile
)
$ErrorActionPreference = 'Stop'
if ($IdfProfile) {
    if (-not (Test-Path -LiteralPath $IdfProfile)) { throw "Missing IDF profile: $IdfProfile" }
    . (Resolve-Path -LiteralPath $IdfProfile).Path
} else {
    if (-not $IdfPath -and (Test-Path -LiteralPath 'C:\esp\v5.5.5\esp-idf\export.ps1')) {
        $IdfPath = 'C:\esp\v5.5.5\esp-idf'
    }
    if (-not $IdfPath) { throw 'Pass -IdfPath for ESP-IDF v5.5.x, for example C:\esp\v5.5.5\esp-idf' }
    $exportScript = Join-Path $IdfPath 'export.ps1'
    if (-not (Test-Path -LiteralPath $exportScript)) { throw "Missing IDF export script: $exportScript" }
    . $exportScript
}
python (Join-Path $PSScriptRoot 'firmware.py') build --board $Board
if ($LASTEXITCODE -ne 0) { throw "Firmware build failed: $LASTEXITCODE" }

param(
    [ValidateSet('v2', 'v1', 'cyd', 'all')][string]$Board = 'v2',
    [string]$IdfPath = $env:IDF_PATH
)
$ErrorActionPreference = 'Stop'
if (-not $IdfPath) { throw 'Pass -IdfPath, for example C:\esp\v6.0.2\esp-idf' }
$exportScript = Join-Path $IdfPath 'export.ps1'
if (-not (Test-Path -LiteralPath $exportScript)) { throw "Missing IDF export script: $exportScript" }
. $exportScript
python (Join-Path $PSScriptRoot 'firmware.py') build --board $Board
if ($LASTEXITCODE -ne 0) { throw "Firmware build failed: $LASTEXITCODE" }

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Toolchain = Get-Content -LiteralPath (Join-Path $RepoRoot 'toolchain.json') -Raw | ConvertFrom-Json
function Resolve-ArduinoCli([string]$Explicit) {
    if ($Explicit) { return $Explicit }
    $found = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    if ($env:LOCALAPPDATA) {
        $candidate = Join-Path $env:LOCALAPPDATA 'Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'Install Arduino CLI 1.5.1 or pass -ArduinoCli <path>.'
}
function Resolve-Python([string]$Explicit) {
    if ($Explicit) { return $Explicit }
    foreach ($name in @('python3','python')) {
        $found = Get-Command $name -ErrorAction SilentlyContinue
        if ($found -and $found.Source -notlike '*WindowsApps*') { return $found.Source }
    }
    if ($env:USERPROFILE) {
        $candidate = Join-Path $env:USERPROFILE '.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw 'Install Python 3 or pass -Python <path>.'
}
function Assert-Exit([string]$Step) { if ($LASTEXITCODE -ne 0) { throw "$Step failed (exit $LASTEXITCODE)." } }

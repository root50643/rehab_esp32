param([Parameter(Mandatory=$true)][string]$Port, [string]$ArduinoCli)
. "$PSScriptRoot/common.ps1"
$cli = Resolve-ArduinoCli $ArduinoCli
$artifacts = Join-Path $RepoRoot 'build/artifacts'
if (!(Test-Path -LiteralPath (Join-Path $artifacts 'RehabEsp32.ino.bin'))) { throw 'Run tools/build.ps1 first.' }
& $cli upload --fqbn $Toolchain.fqbn --port $Port --input-dir $artifacts (Join-Path $RepoRoot 'firmware/RehabEsp32')
Assert-Exit 'Firmware upload'

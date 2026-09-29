param([Parameter(Mandatory=$true)][string]$Port, [string]$ArduinoCli)
. "$PSScriptRoot/common.ps1"
$cli = Resolve-ArduinoCli $ArduinoCli
& $cli monitor --port $Port --config baudrate=115200
Assert-Exit 'Serial monitor'

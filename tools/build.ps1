param([string]$ArduinoCli, [string]$Python, [string]$BuildRoot, [switch]$InstallCore)
. "$PSScriptRoot/common.ps1"
$cli = Resolve-ArduinoCli $ArduinoCli
$py = Resolve-Python $Python
if ($InstallCore) {
    & $cli core update-index --additional-urls $Toolchain.index
    Assert-Exit 'Core index'
    & $cli core install $Toolchain.core --additional-urls $Toolchain.index
    Assert-Exit 'Core install'
}
$installed = (& $cli core list --format json | ConvertFrom-Json).platforms | Where-Object { $_.id -eq 'esp32:esp32' }
Assert-Exit 'Read installed core'
if ($installed.installed_version -ne '3.3.11') { throw 'ESP32 core 3.3.11 required; run build.ps1 -InstallCore.' }
& $py -B (Join-Path $PSScriptRoot 'check_partitions.py')
Assert-Exit 'Source partition layout'
& $py -B (Join-Path $PSScriptRoot 'embed_web.py')
Assert-Exit 'Web asset embedding'
$projectBuild = Join-Path $RepoRoot 'build'
New-Item -ItemType Directory -Path $projectBuild -Force | Out-Null
# Windows Xtensa ld cannot open non-ASCII output paths. Stage source and output
# in an ASCII directory; keep the user's original repository exactly where it is.
if (!$BuildRoot) {
    $identity = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($RepoRoot))).Substring(0,12)
    $BuildRoot = Join-Path ([IO.Path]::GetTempPath()) "rehab-esp32-$identity"
}
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if ($IsWindows -and $BuildRoot -match '[^\x00-\x7F]') { throw 'Use -BuildRoot with an ASCII path, e.g. C:\esp32-build\rehab.' }
$sourceRoot = Join-Path $RepoRoot 'firmware/RehabEsp32'
$stage = Join-Path $BuildRoot 'sketch/RehabEsp32'
$buildPath = Join-Path $BuildRoot 'obj'
$output = Join-Path $BuildRoot 'artifacts'
New-Item -ItemType Directory -Path $stage,$buildPath,$output -Force | Out-Null
foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
    $relative = [IO.Path]::GetRelativePath($stage,$file.FullName)
    if (!(Test-Path -LiteralPath (Join-Path $sourceRoot $relative))) { Remove-Item -LiteralPath $file.FullName }
}
foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Recurse -File) {
    $relative = [IO.Path]::GetRelativePath($sourceRoot,$file.FullName)
    $destination = Join-Path $stage $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
}
& $cli compile --fqbn $Toolchain.fqbn --warnings all --build-path $buildPath --output-dir $output $stage
Assert-Exit 'Firmware compile'
& $py -B (Join-Path $PSScriptRoot 'check_partitions.py') --binary (Join-Path $output 'RehabEsp32.ino.partitions.bin') --sdkconfig (Join-Path $buildPath 'sdkconfig') --boot-app0 (Join-Path $buildPath 'boot_app0.bin') --app (Join-Path $output 'RehabEsp32.ino.bin')
Assert-Exit 'Compiled partition layout and upload regions'
# Arduino's custom partition menu reports total Flash as the maximum size.
# Enforce the actual factory app partition before accepting an upload artifact.
$partitionRows = Get-Content -LiteralPath (Join-Path $sourceRoot 'partitions.csv') | Where-Object { $_ -notmatch '^\s*#' -and $_.Trim() }
$appRows = @($partitionRows | ConvertFrom-Csv -Header 'Name','Type','SubType','Offset','Size','Flags' | Where-Object { $_.Type.Trim() -eq 'app' -and $_.SubType.Trim() -eq 'factory' })
if ($appRows.Count -ne 1) { throw 'Expected exactly one factory app partition.' }
$appSizeText = $appRows[0].Size.Trim()
$appLimit = if ($appSizeText.StartsWith('0x')) { [Convert]::ToInt64($appSizeText.Substring(2),16) } else { [long]$appSizeText }
$appBytes = (Get-Item -LiteralPath (Join-Path $output 'RehabEsp32.ino.bin')).Length
if ($appBytes -gt $appLimit) { throw "Firmware is $appBytes bytes, exceeding the factory partition ($appLimit bytes)." }
Write-Host "Factory app image: $appBytes / $appLimit bytes"
$artifacts = Join-Path $projectBuild 'artifacts'
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
Get-ChildItem -LiteralPath $output -File | Copy-Item -Destination $artifacts -Force
Copy-Item -LiteralPath (Join-Path $RepoRoot 'toolchain.json') -Destination (Join-Path $artifacts 'toolchain.json')
Write-Host "Firmware artifacts: $artifacts"

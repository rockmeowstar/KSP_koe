param([Parameter(Mandatory=$true)][string]$GamePath,
    [string]$OutputDirectory = "$PSScriptRoot\artifacts\physics-precision-build")
$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$managed = Join-Path $GamePath 'KSP_x64_Data\Managed'
$references = @('/nologo', '/optimize+', '/unsafe', '/target:library', "/reference:$managed\Assembly-CSharp.dll")
$references += Get-ChildItem $managed -Filter 'UnityEngine*.dll' | ForEach-Object { "/reference:$($_.FullName)" }
$hardware = Join-Path $PSScriptRoot 'artifacts\package38-build\hardware'
if (-not (Test-Path "$hardware\KoeHardwareProbe.dll") -or -not (Test-Path "$hardware\native.manifest")) {
    throw 'Hardware probe assets missing. Run Build.ps1 first.'
}
$package = Join-Path $OutputDirectory 'GameData\KerbalOverEngineering'
$plugins = Join-Path $package 'Plugins'
New-Item -ItemType Directory -Force $plugins | Out-Null
$options = Join-Path $plugins 'KerbalOverEngineering.Options.dll'
$sources = Get-ChildItem "$PSScriptRoot\core\Options" -Filter '*.cs' | ForEach-Object { $_.FullName }
& $compiler @references "/out:$options" "/resource:$hardware\KoeHardwareProbe.dll,native.file.KoeHardwareProbe.dll" "/resource:$hardware\native.manifest,native.manifest" @sources
if ($LASTEXITCODE -ne 0) { throw 'Options compilation failed' }
$references += @("/reference:$options", "/reference:$GamePath\GameData\000_Harmony\0Harmony.dll")
$payload = Join-Path $OutputDirectory 'KoePhysicsWarpPrecision.Runtime.dll'
& $compiler @references "/out:$payload" "$PSScriptRoot\stock\PhysicsWarpPrecisionFix\PhysicsWarpPrecisionFix.cs" "$PSScriptRoot\stock\PhysicsWarpPrecisionFix\EvaInputFix.cs" "$PSScriptRoot\mod\FAR\PhysicsPrecisionFix\PhysicsPrecisionFix.cs"
if ($LASTEXITCODE -ne 0) { throw 'Precision payload compilation failed' }
$info = Join-Path $OutputDirectory 'module.info'
[IO.File]::WriteAllText($info, "Stock.PhysicsWarpPrecision`nKoePhysicsWarpPrecision.Bootstrap", [Text.UTF8Encoding]::new($false))
$wrapperSource = Join-Path $OutputDirectory 'Bootstrap.cs'
Copy-Item "$PSScriptRoot\core\Options\ModuleBootstrap.cs.template" $wrapperSource
$moduleDirectory = Join-Path $package 'stock\PhysicsWarpPrecisionFix\Plugins'
New-Item -ItemType Directory -Force $moduleDirectory | Out-Null
& $compiler @references "/out:$moduleDirectory\KoePhysicsWarpPrecision.dll" "/resource:$payload,module.bin" "/resource:$info,module.info" $wrapperSource
if ($LASTEXITCODE -ne 0) { throw 'Precision wrapper compilation failed' }
Write-Output "Built incremental files: $package (not a complete release package)"

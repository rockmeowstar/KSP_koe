param([string]$GamePath = $env:KSP_ROOT, [string]$OutputDirectory = "$PSScriptRoot\..\artifacts\physics-precision-checks")
$ErrorActionPreference = 'Stop'
if (-not $GamePath) { throw 'Specify -GamePath or KSP_ROOT' }
$root = Split-Path $PSScriptRoot
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$managed = Join-Path $GamePath 'KSP_x64_Data\Managed'
$references = @('/nologo', '/optimize+', "/reference:$managed\Assembly-CSharp.dll", "/reference:$GamePath\GameData\000_Harmony\0Harmony.dll")
$references += Get-ChildItem $managed -Filter 'UnityEngine*.dll' | ForEach-Object { "/reference:$($_.FullName)" }
$output = Join-Path $OutputDirectory 'PhysicsPrecisionPatchTests.exe'
& $compiler @references "/out:$output" "$root\stock\PhysicsWarpPrecisionFix\PhysicsWarpPrecisionFix.cs" "$root\stock\PhysicsWarpPrecisionFix\EvaInputFix.cs" "$root\mod\FAR\PhysicsPrecisionFix\PhysicsPrecisionFix.cs" "$PSScriptRoot\PhysicsPrecisionPatchTests.cs"
if ($LASTEXITCODE -ne 0) { throw 'Physics precision tests did not compile' }
& $output $GamePath
if ($LASTEXITCODE -ne 0) { throw 'Physics precision checks failed' }

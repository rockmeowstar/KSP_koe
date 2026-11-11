param([string]$OutputDirectory = "$PSScriptRoot\..\artifacts\physics-catchup-checks")
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$executable = Join-Path $OutputDirectory 'PhysicsCatchupTests.exe'
& $compiler /nologo /optimize+ "/out:$executable" "$projectRoot\core\Options\RealtimePhysicsCatchup.cs" "$projectRoot\core\Options\PhysicsWarpPrecision.cs" "$PSScriptRoot\PhysicsCatchupTests.cs"
if ($LASTEXITCODE -ne 0) { throw 'Physics catch-up checks did not compile.' }
& $executable
if ($LASTEXITCODE -ne 0) { throw 'Physics catch-up checks failed.' }

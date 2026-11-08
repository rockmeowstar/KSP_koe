param([string]$GamePath = 'D:/SteamLibrary/steamapps/common/Kerbal Space Program')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot
$testSource = Join-Path $PSScriptRoot 'PlanetLaunchReadinessTests.cs'
$outputDirectory = Join-Path $projectRoot 'artifacts/planet-launch-readiness-tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$managed = Join-Path $GamePath 'KSP_x64_Data/Managed'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$references = @('/nologo','/optimize+',"/reference:$managed/Assembly-CSharp.dll","/reference:$GamePath/GameData/000_Harmony/0Harmony.dll")
$references += Get-ChildItem -LiteralPath $managed -Filter 'UnityEngine*.dll' | ForEach-Object { "/reference:$($_.FullName)" }
$source = Join-Path $projectRoot 'stock/PlanetLaunchReadinessFix/PlanetLaunchReadiness.cs'
& $compiler @references /target:library "/reference:$projectRoot/dist/GameData/KerbalOverEngineering/Plugins/KerbalOverEngineering.Options.dll" "/out:$outputDirectory/KoePlanetLaunchReadiness.dll" $source
if ($LASTEXITCODE -ne 0) { throw 'Production launch readiness module did not compile.' }
& $compiler @references /define:KOE_DIAGNOSTICS /target:exe "/out:$outputDirectory/PlanetLaunchReadinessTests.exe" $source $testSource
if ($LASTEXITCODE -ne 0) { throw 'Launch readiness tests did not compile.' }
& "$outputDirectory/PlanetLaunchReadinessTests.exe" $GamePath
if ($LASTEXITCODE -ne 0) { throw 'Launch readiness IL or predicate test failed.' }

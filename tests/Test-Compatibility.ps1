param([Parameter(Mandatory=$true)][string]$GamePath)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot
$taskOutput=Join-Path $taskRoot 'artifacts\compatibility-tests\matrix'
New-Item -ItemType Directory -Force $taskOutput | Out-Null
$taskCsc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
Copy-Item -LiteralPath "$GamePath\GameData\000_Harmony\0Harmony.dll" -Destination $taskOutput -Force
& $taskCsc /nologo /optimize+ /target:library "/out:$taskOutput\Assembly-CSharp.dll" "$PSScriptRoot\CompatibilityTargets.cs"
if($LASTEXITCODE -ne 0){throw 'Isolated compatibility targets failed to compile'}
& $taskCsc /nologo /optimize+ /target:library "/reference:$taskOutput\0Harmony.dll" "/out:$taskOutput\KSPCommunityFixes.dll" "$PSScriptRoot\CommunityFixesFixture.cs"
if($LASTEXITCODE -ne 0){throw 'Compatibility fixture compilation failed'}
& $taskCsc /nologo /optimize+ "/out:$taskOutput\CompatibilityTests.exe" "$taskRoot\core\Options\CommunityFixes.cs" "$PSScriptRoot\CompatibilityTests.cs"
if($LASTEXITCODE -ne 0){throw 'Compatibility tests compilation failed'}
& "$taskOutput\CompatibilityTests.exe" "$taskOutput\KSPCommunityFixes.dll"
if($LASTEXITCODE -ne 0){throw 'Compatibility matrix failed'}
New-Item -ItemType Directory -Force "$taskOutput\unknown" | Out-Null
& $taskCsc /nologo /optimize+ /define:UNKNOWN_API /target:library "/reference:$taskOutput\0Harmony.dll" "/out:$taskOutput\unknown\KSPCommunityFixes.dll" "$PSScriptRoot\CommunityFixesFixture.cs"
if($LASTEXITCODE -ne 0){throw 'Unknown API fixture compilation failed'}
& "$taskOutput\CompatibilityTests.exe" "$taskOutput\unknown\KSPCommunityFixes.dll" unknown
if($LASTEXITCODE -ne 0){throw 'Unknown API compatibility failed'}
& $taskCsc /nologo /optimize+ "/out:$taskOutput\CoverageGeometryTests.exe" "$taskRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\CoverageVisibility.cs" "$PSScriptRoot\CoverageGeometryTests.cs"
if($LASTEXITCODE -ne 0){throw 'Coverage geometry tests compilation failed'}
& "$taskOutput\CoverageGeometryTests.exe"
if($LASTEXITCODE -ne 0){throw 'Coverage geometry failed'}
& $taskCsc /nologo /optimize+ "/out:$taskOutput\LoadingResourcesTests.exe" "$taskRoot\core\Options\LoadingResources.cs" "$PSScriptRoot\LoadingResourcesTests.cs"
if($LASTEXITCODE -ne 0){throw 'Loading resource tests compilation failed'}
& "$taskOutput\LoadingResourcesTests.exe"
if($LASTEXITCODE -ne 0){throw 'Hardware resource policy failed'}

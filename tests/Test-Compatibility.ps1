param([Parameter(Mandatory = $true)][string]$GamePath)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'artifacts\compatibility-tests\matrix'
New-Item -ItemType Directory -Force $outputDirectory | Out-Null
$csharpCompiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
Copy-Item -LiteralPath "$GamePath\GameData\000_Harmony\0Harmony.dll" -Destination $outputDirectory -Force
& $csharpCompiler /nologo /optimize+ /target:library "/out:$outputDirectory\Assembly-CSharp.dll" "$PSScriptRoot\CompatibilityTargets.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Isolated compatibility targets failed to compile'
}
& $csharpCompiler /nologo /optimize+ /target:library "/reference:$outputDirectory\0Harmony.dll" "/out:$outputDirectory\KSPCommunityFixes.dll" "$PSScriptRoot\CommunityFixesFixture.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Compatibility fixture compilation failed'
}
& $csharpCompiler /nologo /optimize+ "/out:$outputDirectory\CompatibilityTests.exe" "$projectRoot\core\Options\CommunityFixes.cs" "$PSScriptRoot\CompatibilityTests.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Compatibility tests compilation failed'
}
& "$outputDirectory\CompatibilityTests.exe" "$outputDirectory\KSPCommunityFixes.dll"
if ($LASTEXITCODE -ne 0) {
    throw 'Compatibility matrix failed'
}
New-Item -ItemType Directory -Force "$outputDirectory\unknown" | Out-Null
& $csharpCompiler /nologo /optimize+ /define:UNKNOWN_API /target:library "/reference:$outputDirectory\0Harmony.dll" "/out:$outputDirectory\unknown\KSPCommunityFixes.dll" "$PSScriptRoot\CommunityFixesFixture.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Unknown API fixture compilation failed'
}
& "$outputDirectory\CompatibilityTests.exe" "$outputDirectory\unknown\KSPCommunityFixes.dll" unknown
if ($LASTEXITCODE -ne 0) {
    throw 'Unknown API compatibility failed'
}
New-Item -ItemType Directory -Force "$outputDirectory\legacy" | Out-Null
& $csharpCompiler /nologo /optimize+ /define:LEGACY_REPLACEMENTS /target:library "/reference:$outputDirectory\0Harmony.dll" "/out:$outputDirectory\legacy\KSPCommunityFixes.dll" "$PSScriptRoot\CommunityFixesFixture.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Legacy replacement fixture compilation failed'
}
& "$outputDirectory\CompatibilityTests.exe" "$outputDirectory\legacy\KSPCommunityFixes.dll" legacy
if ($LASTEXITCODE -ne 0) {
    throw 'Legacy version default policy failed'
}
& $csharpCompiler /nologo /optimize+ "/out:$outputDirectory\CoverageGeometryTests.exe" "$projectRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\CoverageVisibility.cs" "$PSScriptRoot\CoverageGeometryTests.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Coverage geometry tests compilation failed'
}
& "$outputDirectory\CoverageGeometryTests.exe"
if ($LASTEXITCODE -ne 0) {
    throw 'Coverage geometry failed'
}
& $csharpCompiler /nologo /optimize+ "/out:$outputDirectory\LoadingResourcesTests.exe" "$projectRoot\core\Options\LoadingResources.cs" "$PSScriptRoot\LoadingResourcesTests.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Loading resource tests compilation failed'
}
& "$outputDirectory\LoadingResourcesTests.exe"
if ($LASTEXITCODE -ne 0) {
    throw 'Hardware resource policy failed'
}

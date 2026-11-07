param([string]$GamePath = 'D:/SteamLibrary/steamapps/common/Kerbal Space Program')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'artifacts/event-path-tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$managedDirectory = (Join-Path $GamePath 'KSP_x64_Data\Managed').Replace('/','\')
$harmonyAssembly = (Join-Path $GamePath 'GameData\000_Harmony\0Harmony.dll').Replace('/','\')
$optionsAssembly = Join-Path $projectRoot 'dist\GameData\KerbalOverEngineering\Plugins\KerbalOverEngineering.Options.dll'
$compilerReferences = @('/nologo','/optimize+','/unsafe',"/reference:$managedDirectory\Assembly-CSharp.dll","/reference:$harmonyAssembly","/reference:$optionsAssembly")
$compilerReferences += Get-ChildItem -LiteralPath $managedDirectory -Filter 'UnityEngine*.dll' | ForEach-Object {
    "/reference:$($_.FullName)"
}
$sourceFiles = @((Join-Path $PSScriptRoot 'EventPathTests.cs'))
foreach ($sourceDirectory in @('stock/EventPathFix','stock/PropellantRequestFix','mod/KSPCommunityFixes/CollisionStateFix','mod/FreeIva/RefreshFix','mod/Waterfall/RendererIndexFix','mod/FerramAerospaceResearch/EventPathFix')) {
    $sourceFiles += Get-ChildItem -LiteralPath (Join-Path $projectRoot $sourceDirectory) -Filter '*.cs' | ForEach-Object {
        $_.FullName
    }
}
& "$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe" @compilerReferences "/out:$(Join-Path $outputDirectory 'EventPathTests.exe')" @sourceFiles
if ($LASTEXITCODE -ne 0) {
    throw 'Event path test compilation failed'
}
& "$outputDirectory/EventPathTests.exe" $GamePath $projectRoot
if ($LASTEXITCODE -ne 0) {
    throw 'Event path static tests failed'
}

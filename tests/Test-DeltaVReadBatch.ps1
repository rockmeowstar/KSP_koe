param([string]$GamePath = 'D:/SteamLibrary/steamapps/common/Kerbal Space Program')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'artifacts/delta-v-read-batch-tests'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$managedDirectory = (Join-Path $GamePath 'KSP_x64_Data\Managed').Replace('/','\')
$compilerReferences = @('/nologo','/optimize+',"/reference:$managedDirectory\Assembly-CSharp.dll","/reference:$GamePath/GameData/000_Harmony/0Harmony.dll","/reference:$projectRoot/dist/GameData/KerbalOverEngineering/Plugins/KerbalOverEngineering.Options.dll")
$compilerReferences += Get-ChildItem -LiteralPath $managedDirectory -Filter 'UnityEngine*.dll' | ForEach-Object {
    "/reference:$($_.FullName)"
}
$sourceFiles = @((Join-Path $PSScriptRoot 'DeltaVReadBatchTests.cs'))
$sourceFiles += Get-ChildItem -LiteralPath (Join-Path $projectRoot 'stock/DeltaVReadBatchFix') -Filter '*.cs' | ForEach-Object {
    $_.FullName
}
& "$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe" @compilerReferences "/out:$(Join-Path $outputDirectory 'DeltaVReadBatchTests.exe')" @sourceFiles
if ($LASTEXITCODE -ne 0) {
    throw 'Could not compile the delta-V batching tests.'
}
& "$outputDirectory/DeltaVReadBatchTests.exe" $GamePath
if ($LASTEXITCODE -ne 0) {
    throw 'A delta-V batching test failed; see the scenario above.'
}

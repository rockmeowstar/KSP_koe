param([string]$GamePath='D:/SteamLibrary/steamapps/common/Kerbal Space Program')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
$output=Join-Path $repo 'artifacts/stage-delta-v-tests'
New-Item -ItemType Directory -Force $output | Out-Null
$managed=(Join-Path $GamePath 'KSP_x64_Data/Managed').Replace('/','\')
$game=$GamePath.Replace('/','\')
$refs=@('/nologo','/optimize+',"/reference:$managed\Assembly-CSharp.dll","/reference:$game\GameData\000_Harmony\0Harmony.dll","/reference:$repo\dist\GameData\KerbalOverEngineering\Plugins\KerbalOverEngineering.Options.dll")
$refs+=Get-ChildItem -LiteralPath $managed -Filter 'UnityEngine*.dll'|ForEach-Object{"/reference:$($_.FullName)"}
& "$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe" @refs "/out:$output\StageDeltaVTests.exe" "$PSScriptRoot\StageDeltaVTests.cs" "$repo\stock\StageDeltaVFix\StageDeltaVFix.cs"
if($LASTEXITCODE -ne 0){throw 'Stage tests compilation failed'}
& "$output/StageDeltaVTests.exe" $GamePath $repo
if($LASTEXITCODE -ne 0){throw 'Stage tests failed'}

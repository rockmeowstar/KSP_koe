param([string]$GamePath=$env:KSP_ROOT)
$ErrorActionPreference='Stop'
if(!$GamePath){throw 'Specify -GamePath or KSP_ROOT'}
$repo=Split-Path $PSScriptRoot
$output=Join-Path $repo 'artifacts\far-exposure-pool-tests'
New-Item -ItemType Directory -Force $output | Out-Null
$game=$GamePath.Replace('/','\');$managed="$game\KSP_x64_Data\Managed"
$refs=@('/nologo','/optimize+',"/reference:$managed\Assembly-CSharp.dll","/reference:$game\GameData\000_Harmony\0Harmony.dll","/reference:$game\GameData\KerbalOverEngineering\Plugins\KerbalOverEngineering.Options.dll")
$refs+=Get-ChildItem -LiteralPath $managed -Filter 'UnityEngine*.dll'|ForEach-Object{"/reference:$($_.FullName)"}
& "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" @refs "/out:$output\FarExposurePoolTests.exe" "$PSScriptRoot\FarExposurePoolTests.cs" "$repo\mod\FerramAerospaceResearch\EventPathFix\ExposurePool.cs"
if($LASTEXITCODE -ne 0){throw 'FAR pool tests compilation failed'}
& "$output\FarExposurePoolTests.exe" $GamePath $repo
if($LASTEXITCODE -ne 0){throw 'FAR pool tests failed'}

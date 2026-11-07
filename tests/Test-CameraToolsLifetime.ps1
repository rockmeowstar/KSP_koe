param([string]$GamePath=$env:KSP_ROOT)
$ErrorActionPreference='Stop'
if(!$GamePath){throw 'Specify -GamePath or KSP_ROOT'}
$repo=Split-Path $PSScriptRoot
$output=Join-Path $repo 'artifacts\camera-tools-lifetime-tests'
New-Item -ItemType Directory -Force $output | Out-Null
$game=$GamePath.Replace('/','\');$managed="$game\KSP_x64_Data\Managed"
$refs=@('/nologo','/optimize+',"/reference:$managed\Assembly-CSharp.dll","/reference:$game\GameData\000_Harmony\0Harmony.dll","/reference:$game\GameData\KerbalOverEngineering\Plugins\KerbalOverEngineering.Options.dll")
$refs+=Get-ChildItem -LiteralPath $managed -Filter 'UnityEngine*.dll' | ForEach-Object {"/reference:$($_.FullName)"}
& "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" @refs "/out:$output\CameraToolsLifetimeTests.exe" "$PSScriptRoot\CameraToolsLifetimeTests.cs" "$repo\mod\CameraTools\ToolbarLifetimeFix\ToolbarLifetimeFix.cs"
if($LASTEXITCODE -ne 0){throw 'CameraTools lifetime compilation failed'}
& "$output\CameraToolsLifetimeTests.exe" $GamePath
if($LASTEXITCODE -ne 0){throw 'CameraTools lifetime tests failed'}

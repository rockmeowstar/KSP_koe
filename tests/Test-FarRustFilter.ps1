param([string]$GamePath='D:\SteamLibrary\steamapps\common\Kerbal Space Program',[string]$Library)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
if(!$Library){$Library="$repo\mod\FerramAerospaceResearch\EventPathFix\native-assets\KoeFarRendererFilter.dll"}
New-Item -ItemType Directory -Force -Path "$repo\artifacts\far-rust-filter"|Out-Null
$out="$repo\artifacts\far-rust-filter\FarRustFilterTests.exe"
$managed=Join-Path $GamePath 'KSP_x64_Data\Managed'
$refs=@('/nologo','/optimize+','/unsafe',"/reference:$managed\Assembly-CSharp.dll","/reference:$GamePath\GameData\000_Harmony\0Harmony.dll")
$refs+="/reference:$repo\dist\GameData\KerbalOverEngineering\Plugins\KerbalOverEngineering.Options.dll"
$refs+=Get-ChildItem -LiteralPath $managed -Filter 'UnityEngine*.dll'|ForEach-Object{"/reference:$($_.FullName)"}
& "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe" @refs "/out:$out" "$PSScriptRoot\FarRustFilterTests.cs" "$repo\mod\FerramAerospaceResearch\EventPathFix\RustRendererFilter.cs" "$repo\mod\FerramAerospaceResearch\EventPathFix\RustRendererSetup.cs"
if($LASTEXITCODE -ne 0){throw 'FAR Rust bridge test compilation failed'}
& $out $GamePath $Library
if($LASTEXITCODE -ne 0){throw 'FAR Rust bridge tests failed'}

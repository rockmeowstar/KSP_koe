param([string]$Rustc='rustc',[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
if(!$OutputDirectory){$OutputDirectory=Join-Path $repo 'artifacts/cloud-fusion-tests'}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
. "$repo/core/BuildTools.ps1";$tools=Get-KoeBuildTools
$savedLib=$env:LIB
try{
 $env:LIB="$($tools.Compiler)/lib/x64;$($tools.SDK)/Lib/$($tools.Version)/ucrt/x64;$($tools.SDK)/Lib/$($tools.Version)/um/x64"
 & $Rustc --edition=2021 --test -C opt-level=3 -C target-feature=+crt-static "-Clinker=$($tools.Compiler)/bin/Hostx64/x64/link.exe" "$repo/mod/EnvironmentalVisualEnhancements/RustCloudRenderer/Compiler.rs" -o "$OutputDirectory/CloudCompilerTests.exe"
 if($LASTEXITCODE){throw 'Cloud compiler test build failed'}
 & "$OutputDirectory/CloudCompilerTests.exe" --quiet
 if($LASTEXITCODE){throw 'Cloud compiler tests failed'}
}finally{$env:LIB=$savedLib}

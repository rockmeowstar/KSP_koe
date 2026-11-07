param([Parameter(Mandatory=$true)][string]$OutputDirectory,[string]$Rustc='rustc')
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path (Split-Path $PSScriptRoot))
. "$repo\core\BuildTools.ps1"
$tools=Get-KoeBuildTools
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$savedLib=$env:LIB
try {
    $env:LIB="$($tools.Compiler)\lib\x64;$($tools.SDK)\Lib\$($tools.Version)\ucrt\x64;$($tools.SDK)\Lib\$($tools.Version)\um\x64"
    & $Rustc --edition=2021 -C opt-level=3 -C target-feature=+crt-static "-Clinker=$($tools.Compiler)\bin\Hostx64\x64\link.exe" "$PSScriptRoot\InspectShader.rs" -o "$OutputDirectory\InspectEveShader.exe"
    if($LASTEXITCODE -ne 0){throw 'Rust cloud shader inspector build failed'}
    & $Rustc --edition=2021 --test -C opt-level=3 -C target-feature=+crt-static "-Clinker=$($tools.Compiler)\bin\Hostx64\x64\link.exe" "$PSScriptRoot\ShaderProgram.rs" -o "$OutputDirectory\CloudShaderTests.exe"
    if($LASTEXITCODE -ne 0){throw 'Rust cloud shader tests build failed'}
    & "$OutputDirectory\CloudShaderTests.exe"
    if($LASTEXITCODE -ne 0){throw 'Rust cloud shader tests failed'}
}finally{$env:LIB=$savedLib}

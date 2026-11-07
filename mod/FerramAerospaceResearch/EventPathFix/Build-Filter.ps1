param([string]$OutputDirectory,[string]$Rustc='rustc',[switch]$InstallAsset)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path (Split-Path $PSScriptRoot))
. "$repo\core\BuildTools.ps1"
$tools=Get-KoeBuildTools
if(!$OutputDirectory){$OutputDirectory="$repo\artifacts\far-rust-filter"}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$linker="$($tools.Compiler)\bin\Hostx64\x64\link.exe"
$libs=@("$($tools.Compiler)\lib\x64","$($tools.SDK)\Lib\$($tools.Version)\ucrt\x64","$($tools.SDK)\Lib\$($tools.Version)\um\x64")
$flags=@('-C',"linker=$linker")
foreach($lib in $libs){$flags+=@('-C',"link-arg=/LIBPATH:$lib")}
& $Rustc --edition=2021 --crate-type cdylib -C opt-level=3 -C panic=abort -C target-cpu=x86-64 @flags "$PSScriptRoot\RendererFilter.rs" -o "$OutputDirectory\KoeFarRendererFilter.dll"
if($LASTEXITCODE -ne 0){throw 'Rust renderer filter build failed'}
& $Rustc --edition=2021 --test -C opt-level=3 @flags "$PSScriptRoot\RendererFilter.rs" -o "$OutputDirectory\RendererFilterTests.exe"
if($LASTEXITCODE -ne 0){throw 'Rust renderer filter tests compilation failed'}
& "$OutputDirectory\RendererFilterTests.exe"
if($LASTEXITCODE -ne 0){throw 'Rust renderer filter tests failed'}
if($InstallAsset){
    $assets=Join-Path $PSScriptRoot 'native-assets'
    New-Item -ItemType Directory -Path $assets -Force|Out-Null
    Copy-Item -LiteralPath "$OutputDirectory\KoeFarRendererFilter.dll" -Destination "$assets\KoeFarRendererFilter.dll" -Force
    $sources=@{}
    foreach($name in @('RendererFilter.rs','Build-Filter.ps1')){$sources[$name]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name)).Hash.ToLowerInvariant()}
    $manifest=@{binarySha256=(Get-FileHash -LiteralPath "$assets\KoeFarRendererFilter.dll").Hash.ToLowerInvariant();sources=$sources}
    [IO.File]::WriteAllText("$assets\KoeFarRendererFilter.build.json",($manifest|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
}

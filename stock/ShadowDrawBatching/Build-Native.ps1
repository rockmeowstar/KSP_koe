param([string]$OutputDirectory,[string]$Rustc='rustc',[switch]$TestFailures)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot)
. "$repo/core/BuildTools.ps1"
$tools=Get-KoeBuildTools
if(!$OutputDirectory){$OutputDirectory=Join-Path $repo 'artifacts/shadow-batches'}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$savedLib=$env:LIB
try{
 $env:LIB="$($tools.Compiler)/lib/x64;$($tools.SDK)/Lib/$($tools.Version)/ucrt/x64;$($tools.SDK)/Lib/$($tools.Version)/um/x64"
 & $Rustc --edition=2021 --crate-type staticlib --crate-name koe_shadow_queue -C opt-level=3 -C panic=abort -C target-feature=+crt-static "$PSScriptRoot/ShadowQueue.rs" -o "$OutputDirectory/ShadowQueue.lib"
 if($LASTEXITCODE){throw 'Rust shadow queue compilation failed'}
}finally{$env:LIB=$savedLib}
python "$PSScriptRoot/Generate-ShadowHooks.py" --sdk "$($tools.SDK)/Include/$($tools.Version)/um" --output "$OutputDirectory/ShadowContextHooks.generated.h"
if($LASTEXITCODE){throw 'Shadow context boundary generation failed'}
$includes=@($OutputDirectory,"$($tools.Compiler)/include","$($tools.SDK)/Include/$($tools.Version)/ucrt","$($tools.SDK)/Include/$($tools.Version)/shared","$($tools.SDK)/Include/$($tools.Version)/um","$($tools.SDK)/Include/$($tools.Version)/winrt","$repo/third_party/minhook/include")|ForEach-Object{"/I$_"}
$libs=@("$($tools.Compiler)/lib/x64","$($tools.SDK)/Lib/$($tools.Version)/ucrt/x64","$($tools.SDK)/Lib/$($tools.Version)/um/x64")|ForEach-Object{"/LIBPATH:$_"}
$sources=@('buffer.c','hook.c','trampoline.c','hde/hde64.c')|ForEach-Object{"$repo/third_party/minhook/src/$_"}
$defines=@();if($TestFailures){$defines+='/DKOE_SHADOW_TEST'}
Push-Location $OutputDirectory
try{
 & "$($tools.Compiler)/bin/Hostx64/x64/cl.exe" /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @defines @includes "$PSScriptRoot/NativeShadowBatches.cpp" @sources ShadowQueue.lib /link @libs d3d11.lib dxgi.lib bcrypt.lib advapi32.lib userenv.lib ws2_32.lib ntdll.lib /OUT:KoeShadowBatches.dll
 if($LASTEXITCODE){throw 'Shadow native compilation failed'}
}finally{Pop-Location}

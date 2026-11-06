param([string]$OutputDirectory, [switch]$Audit, [string]$RustKernelHeader, [switch]$AssemblyOnly, [switch]$RebuildRust, [string]$Rustc='rustc')
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot)
. "$repo\core\BuildTools.ps1"
$tools=Get-KoeBuildTools
if(!$OutputDirectory){$OutputDirectory=Join-Path $repo 'artifacts\shader-constants-production'}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
if($RebuildRust){
    if($AssemblyOnly){throw 'RebuildRust and AssemblyOnly are mutually exclusive'}
    & "$PSScriptRoot\Build-Rust.ps1" -Rustc $Rustc -OutputDirectory "$OutputDirectory\rust"
}
$include=@("$($tools.Compiler)\include","$($tools.SDK)\Include\$($tools.Version)\ucrt","$($tools.SDK)\Include\$($tools.Version)\shared","$($tools.SDK)\Include\$($tools.Version)\um","$($tools.SDK)\Include\$($tools.Version)\winrt","$repo\third_party\minhook\include","$repo\third_party\minhook\src")|ForEach-Object{"/I$_"}
$libs=@("$($tools.Compiler)\lib\x64","$($tools.SDK)\Lib\$($tools.Version)\ucrt\x64","$($tools.SDK)\Lib\$($tools.Version)\um\x64")|ForEach-Object{"/LIBPATH:$_"}
$hook=[IO.File]::ReadAllText("$repo\third_party\minhook\src\hook.c")
function Replace-One([string]$old,[string]$new){
    if(($script:hook.Split(@($old),[StringSplitOptions]::None).Count-1) -ne 1){throw 'MinHook source changed'}
    $script:hook=$script:hook.Replace($old,$new)
}
Replace-One '#include "../include/MinHook.h"' '#include "MinHook.h"'
Replace-One 'UINT8  patchAbove  : 1;' 'UINT8 isCall : 1; UINT8 patchAbove : 1;'
Replace-One 'BOOL        enable;' 'BOOL enable; if (pHook->isCall) continue;'
Replace-One 'pJmp->opcode = 0xE9;' 'pJmp->opcode = pHook->isCall ? 0xE8 : 0xE9;'
Replace-One 'pHook->patchAbove  = ct.patchAbove;' 'pHook->isCall = FALSE; pHook->patchAbove = ct.patchAbove;'
$hook+="`n"+[IO.File]::ReadAllText("$PSScriptRoot\CallHookExtension.c")
[IO.File]::WriteAllText("$OutputDirectory\CallHooks.c",$hook,[Text.UTF8Encoding]::new($false))
$sources=@('buffer.c','trampoline.c','hde\hde64.c')|ForEach-Object{"$repo\third_party\minhook\src\$_"}
$sources+="$OutputDirectory\CallHooks.c"
$defines=@();if($Audit){$defines+='/DKOE_UNIFORM_AUDIT'}
if(!$RustKernelHeader -and !$AssemblyOnly){$RustKernelHeader=Join-Path $PSScriptRoot 'RustKernel.h'}
if($RustKernelHeader){
    $header=[IO.File]::ReadAllText($RustKernelHeader)
    $sourceHash=(Get-FileHash "$PSScriptRoot\UniformUpdate.rs" -Algorithm SHA256).Hash.ToLowerInvariant()
    if(!$header.Contains('RustKernelSourceHash[]="'+$sourceHash+'"')){throw 'Rust source/header mismatch; regenerate with -RebuildRust'}
    Copy-Item -LiteralPath $RustKernelHeader -Destination "$OutputDirectory\RustKernel.h"
    $include+="/I$OutputDirectory";$defines+='/DKOE_RUST_KERNEL'
}
Push-Location $OutputDirectory
try{
    & "$($tools.Compiler)\bin\Hostx64\x64\ml64.exe" /nologo /c /FoUniformUpdate.obj "$PSScriptRoot\UniformUpdate.asm"
    if($LASTEXITCODE -ne 0){throw 'Shader constant assembly failed'}
    & "$($tools.Compiler)\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @defines @include "$PSScriptRoot\NativeShaderConstants.cpp" UniformUpdate.obj @sources /link @libs bcrypt.lib /OUT:KoeUniformKernel.dll
    if($LASTEXITCODE -ne 0){throw 'Shader constant native compilation failed'}
}finally{Pop-Location}
if(!$Audit){
    $assets=Join-Path $PSScriptRoot 'native-assets';New-Item -ItemType Directory -Path $assets -Force | Out-Null
    Copy-Item -LiteralPath "$OutputDirectory\KoeUniformKernel.dll" -Destination "$assets\KoeUniformKernel.dll"
}

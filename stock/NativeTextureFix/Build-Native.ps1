param([string]$OutputPath)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path (Split-Path $PSScriptRoot)
if(!$OutputPath){$OutputPath=Join-Path $taskRoot 'artifacts\native-texture-production\NativeTextureEncoder.bin'}
$OutputPath=[IO.Path]::GetFullPath($OutputPath)
$taskCompiler='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
$taskSdk='C:\Program Files (x86)\Windows Kits\10'
$taskSdkVersion='10.0.26100.0'
$taskBuild=Split-Path $OutputPath
New-Item -ItemType Directory -Force -Path $taskBuild | Out-Null
$taskOldPath=$env:PATH
try{
    $env:PATH=(Join-Path $taskCompiler 'bin\Hostx64\x64')+';'+$taskOldPath
    $taskIncludes=@((Join-Path $taskCompiler 'include'),(Join-Path $taskSdk "Include\$taskSdkVersion\ucrt"),(Join-Path $taskSdk "Include\$taskSdkVersion\shared"),(Join-Path $taskSdk "Include\$taskSdkVersion\um"),(Join-Path $taskRoot 'third_party\minhook\include')) | ForEach-Object {"/I$_"}
    $taskLibs=@((Join-Path $taskCompiler 'lib\x64'),(Join-Path $taskSdk "Lib\$taskSdkVersion\ucrt\x64"),(Join-Path $taskSdk "Lib\$taskSdkVersion\um\x64")) | ForEach-Object {"/LIBPATH:$_"}
    $taskSources=@('buffer.c','hook.c','trampoline.c','hde\hde64.c') | ForEach-Object {Join-Path $taskRoot "third_party\minhook\src\$_"}
    Push-Location $taskBuild
    try{
        & cl.exe /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @taskIncludes (Join-Path $PSScriptRoot 'NativeTextureEncoder.cpp') @taskSources /link @taskLibs "/OUT:$OutputPath"
        if($LASTEXITCODE -ne 0){throw 'Native texture encoder build failed'}
    }finally{Pop-Location}
}finally{$env:PATH=$taskOldPath}

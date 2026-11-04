param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path (Split-Path $PSScriptRoot)
$taskCompiler='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
$taskSdk='C:\Program Files (x86)\Windows Kits\10'
$taskVersion='10.0.26100.0'
if(-not $OutputDirectory){$OutputDirectory=Join-Path $taskRoot 'artifacts\point-lights-production'}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$taskIncludes=@("$taskCompiler\include","$taskSdk\Include\$taskVersion\ucrt","$taskSdk\Include\$taskVersion\shared","$taskSdk\Include\$taskVersion\um","$taskSdk\Include\$taskVersion\winrt","$taskRoot\third_party\minhook\include",$PSScriptRoot) | ForEach-Object {"/I$_"}
$taskLibraries=@("$taskCompiler\lib\x64","$taskSdk\Lib\$taskVersion\ucrt\x64","$taskSdk\Lib\$taskVersion\um\x64") | ForEach-Object {"/LIBPATH:$_"}
$taskMinHook=@('buffer.c','hook.c','trampoline.c','hde\hde64.c') | ForEach-Object {Join-Path $taskRoot "third_party\minhook\src\$_"}
Push-Location $OutputDirectory
try {
 & "$taskCompiler\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @taskIncludes "$PSScriptRoot\NativeInstancedLights.cpp" "$PSScriptRoot\InstancedLightBatch.cpp" @taskMinHook /link @taskLibraries d3d11.lib dxgi.lib bcrypt.lib /OUT:KoeInstancedLights.dll
 if($LASTEXITCODE -ne 0){throw 'Production point-light compilation failed'}
} finally {Pop-Location}

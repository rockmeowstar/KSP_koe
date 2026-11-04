param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path (Split-Path (Split-Path $PSScriptRoot))
. "$taskRoot\core\BuildTools.ps1"
$taskTools=Get-KoeBuildTools
$taskCompiler=$taskTools.Compiler
$taskSdk=$taskTools.SDK
$taskVersion=$taskTools.Version
if(-not $OutputDirectory){$OutputDirectory=Join-Path $taskRoot 'artifacts\cloud-guards-production'}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$taskIncludes=@("$taskCompiler\include","$taskSdk\Include\$taskVersion\ucrt","$taskSdk\Include\$taskVersion\shared","$taskSdk\Include\$taskVersion\um","$taskRoot\third_party\minhook\include","$taskRoot\core\ShaderPatch") | ForEach-Object {"/I$_"}
$taskLibraries=@("$taskCompiler\lib\x64","$taskSdk\Lib\$taskVersion\ucrt\x64","$taskSdk\Lib\$taskVersion\um\x64") | ForEach-Object {"/LIBPATH:$_"}
$taskMinHook=@('buffer.c','hook.c','trampoline.c','hde\hde64.c') | ForEach-Object {Join-Path $taskRoot "third_party\minhook\src\$_"}
Push-Location $OutputDirectory
try {
 & "$taskCompiler\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @taskIncludes "$PSScriptRoot\NativeCloudGuard.cpp" @taskMinHook /link @taskLibraries d3d11.lib bcrypt.lib /OUT:KoeCloudGuard.dll
 if($LASTEXITCODE -ne 0){throw 'Cloud recipe native compilation failed'}
 & "$taskCompiler\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /EHsc /std:c++17 @taskIncludes "$taskRoot\tests\ShaderPatchTests.cpp" /link @taskLibraries bcrypt.lib /OUT:ShaderPatchTests.exe
 if($LASTEXITCODE -ne 0){throw 'Shader recipe tests compilation failed'}
} finally {Pop-Location}

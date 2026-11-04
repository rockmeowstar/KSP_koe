param([string]$OutputDirectory)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path (Split-Path $PSScriptRoot)
. "$taskRoot\core\BuildTools.ps1"
$taskTools=Get-KoeBuildTools
$taskCompiler=$taskTools.Compiler
$taskSdk=$taskTools.SDK
$taskVersion=$taskTools.Version
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$taskIncludes=@("$taskCompiler\include","$taskSdk\Include\$taskVersion\ucrt","$taskSdk\Include\$taskVersion\shared","$taskSdk\Include\$taskVersion\um","$taskSdk\Include\$taskVersion\winrt","$taskRoot\third_party\nvidia-optical-flow-headers") | ForEach-Object {"/I$_"}
$taskLibraries=@("$taskCompiler\lib\x64","$taskSdk\Lib\$taskVersion\ucrt\x64","$taskSdk\Lib\$taskVersion\um\x64") | ForEach-Object {"/LIBPATH:$_"}
Push-Location $OutputDirectory
try {
 & "$taskCompiler\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @taskIncludes "$PSScriptRoot\HardwareProbe.cpp" /link @taskLibraries d3d11.lib dxgi.lib /OUT:KoeHardwareProbe.dll
 if($LASTEXITCODE -ne 0){throw 'Hardware probe compilation failed'}
} finally {Pop-Location}

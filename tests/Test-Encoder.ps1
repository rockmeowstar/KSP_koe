$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot
. "$taskRoot\core\BuildTools.ps1"
$taskTools=Get-KoeBuildTools
$taskCompiler=$taskTools.Compiler;$taskSdk=$taskTools.SDK;$taskVersion=$taskTools.Version
$taskOutput=Join-Path $taskRoot 'artifacts\compatibility-tests\encoder'
New-Item -ItemType Directory -Force $taskOutput | Out-Null
$taskIncludes=@("$taskCompiler\include","$taskSdk\Include\$taskVersion\ucrt","$taskSdk\Include\$taskVersion\shared","$taskSdk\Include\$taskVersion\um","$taskRoot\stock\NativeTextureFix") | ForEach-Object {"/I$_"}
$taskLibraries=@("$taskCompiler\lib\x64","$taskSdk\Lib\$taskVersion\ucrt\x64","$taskSdk\Lib\$taskVersion\um\x64") | ForEach-Object {"/LIBPATH:$_"}
Push-Location $taskOutput
try{
 & "$taskCompiler\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /EHsc /std:c++17 @taskIncludes "$PSScriptRoot\EncoderLocatorTests.cpp" /link @taskLibraries /OUT:EncoderLocatorTests.exe
 if($LASTEXITCODE -ne 0){throw 'Encoder locator test compilation failed'}
 & .\EncoderLocatorTests.exe
 if($LASTEXITCODE -ne 0){throw 'Encoder locator tests failed'}
}finally{Pop-Location}
$taskCsc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $taskCsc /nologo /optimize+ /platform:x64 "/out:$taskOutput\EncoderPresenceTests.exe" "$PSScriptRoot\EncoderPresenceTests.cs"
if($LASTEXITCODE -ne 0){throw 'Encoder presence test compilation failed'}
& "$taskOutput\EncoderPresenceTests.exe" "$taskRoot\artifacts\native-texture-production\NativeTextureEncoder.bin"
if($LASTEXITCODE -ne 0){throw 'Missing encoder availability check failed'}

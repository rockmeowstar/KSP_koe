param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path (Split-Path $PSScriptRoot))
. "$projectRoot\core\BuildTools.ps1"
$buildTools = Get-KoeBuildTools
$compilerDirectory = $buildTools.Compiler
$windowsSdk = $buildTools.SDK
$sdkVersion = $buildTools.Version
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot 'artifacts\cloud-guards-production'
}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$includePaths = @("$compilerDirectory\include","$windowsSdk\Include\$sdkVersion\ucrt","$windowsSdk\Include\$sdkVersion\shared","$windowsSdk\Include\$sdkVersion\um","$projectRoot\third_party\minhook\include","$projectRoot\core\ShaderPatch") | ForEach-Object {
    "/I$_"
}
$libraryPaths = @("$compilerDirectory\lib\x64","$windowsSdk\Lib\$sdkVersion\ucrt\x64","$windowsSdk\Lib\$sdkVersion\um\x64") | ForEach-Object {
    "/LIBPATH:$_"
}
$minHookSources = @('buffer.c','hook.c','trampoline.c','hde\hde64.c') | ForEach-Object {
    Join-Path $projectRoot "third_party\minhook\src\$_"
}
Push-Location $OutputDirectory
try {
    & "$compilerDirectory\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @includePaths "$PSScriptRoot\NativeCloudGuard.cpp" @minHookSources /link @libraryPaths d3d11.lib bcrypt.lib /OUT:KoeCloudGuard.dll
    if ($LASTEXITCODE -ne 0) {
        throw 'Cloud recipe native compilation failed'
    }
    & "$compilerDirectory\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /EHsc /std:c++17 @includePaths "$projectRoot\tests\ShaderPatchTests.cpp" /link @libraryPaths bcrypt.lib /OUT:ShaderPatchTests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Shader recipe tests compilation failed'
    }
}
finally {
    Pop-Location
}

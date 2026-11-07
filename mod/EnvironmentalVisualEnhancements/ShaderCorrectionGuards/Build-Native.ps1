param([string]$OutputDirectory,[string]$Rustc='rustc')
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
$savedLib = $env:LIB
try {
    $env:LIB="$compilerDirectory\lib\x64;$windowsSdk\Lib\$sdkVersion\ucrt\x64;$windowsSdk\Lib\$sdkVersion\um\x64"
    & $Rustc --edition=2021 --crate-type=staticlib -C opt-level=3 -C target-feature=+crt-static "-Clinker=$compilerDirectory\bin\Hostx64\x64\link.exe" "$PSScriptRoot\..\RustCloudRenderer\Compiler.rs" -o "$OutputDirectory\RustCloudCompiler.lib"
    if ($LASTEXITCODE -ne 0) { throw 'Rust cloud compiler build failed' }
} finally { $env:LIB=$savedLib }
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
    & "$compilerDirectory\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @includePaths "/I$projectRoot\third_party\DxbcHash" /FIWindows.h "$PSScriptRoot\NativeCloudGuard.cpp" "$projectRoot\third_party\DxbcHash\DxilHash.cpp" @minHookSources RustCloudCompiler.lib /link @libraryPaths d3d11.lib bcrypt.lib advapi32.lib userenv.lib ws2_32.lib ntdll.lib /OUT:KoeCloudGuard.dll
    if ($LASTEXITCODE -ne 0) {
        throw 'Cloud recipe native compilation failed'
    }
    & "$compilerDirectory\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /EHsc /std:c++17 @includePaths "$projectRoot\tests\ShaderPatchTests.cpp" /link @libraryPaths bcrypt.lib /OUT:ShaderPatchTests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Shader recipe tests compilation failed'
    }
    & "$compilerDirectory\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /EHsc /std:c++17 @includePaths "$projectRoot\tests\CloudPackingTests.cpp" /link @libraryPaths d3d11.lib /OUT:CloudPackingTests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Cloud packing adapter tests compilation failed' }
}
finally {
    Pop-Location
}

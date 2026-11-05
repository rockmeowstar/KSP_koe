$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot
. "$projectRoot\core\BuildTools.ps1"
$buildTools = Get-KoeBuildTools
$compilerDirectory = $buildTools.Compiler
$windowsSdk = $buildTools.SDK
$sdkVersion = $buildTools.Version
$outputDirectory = Join-Path $projectRoot 'artifacts\compatibility-tests\encoder'
New-Item -ItemType Directory -Force $outputDirectory | Out-Null
$includePaths = @("$compilerDirectory\include","$windowsSdk\Include\$sdkVersion\ucrt","$windowsSdk\Include\$sdkVersion\shared","$windowsSdk\Include\$sdkVersion\um","$projectRoot\stock\NativeTextureFix") | ForEach-Object {
    "/I$_"
}
$libraryPaths = @("$compilerDirectory\lib\x64","$windowsSdk\Lib\$sdkVersion\ucrt\x64","$windowsSdk\Lib\$sdkVersion\um\x64") | ForEach-Object {
    "/LIBPATH:$_"
}
Push-Location $outputDirectory
try {
    & "$compilerDirectory\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /EHsc /std:c++17 @includePaths "$PSScriptRoot\EncoderLocatorTests.cpp" /link @libraryPaths /OUT:EncoderLocatorTests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Encoder locator test compilation failed'
    }
    & .\EncoderLocatorTests.exe
    if ($LASTEXITCODE -ne 0) {
        throw 'Encoder locator tests failed'
    }
}
finally {
    Pop-Location
}
$csharpCompiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $csharpCompiler /nologo /optimize+ /platform:x64 "/out:$outputDirectory\EncoderPresenceTests.exe" "$PSScriptRoot\EncoderPresenceTests.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Encoder presence test compilation failed'
}
& "$outputDirectory\EncoderPresenceTests.exe" "$projectRoot\artifacts\native-texture-production\NativeTextureEncoder.bin"
if ($LASTEXITCODE -ne 0) {
    throw 'Missing encoder availability check failed'
}

param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot)
. "$projectRoot\core\BuildTools.ps1"
$buildTools = Get-KoeBuildTools
$compilerDirectory = $buildTools.Compiler
$windowsSdk = $buildTools.SDK
$sdkVersion = $buildTools.Version
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$includePaths = @("$compilerDirectory\include","$windowsSdk\Include\$sdkVersion\ucrt","$windowsSdk\Include\$sdkVersion\shared","$windowsSdk\Include\$sdkVersion\um","$windowsSdk\Include\$sdkVersion\winrt","$projectRoot\third_party\nvidia-optical-flow-headers") | ForEach-Object {
    "/I$_"
}
$libraryPaths = @("$compilerDirectory\lib\x64","$windowsSdk\Lib\$sdkVersion\ucrt\x64","$windowsSdk\Lib\$sdkVersion\um\x64") | ForEach-Object {
    "/LIBPATH:$_"
}
Push-Location $OutputDirectory
try {
    & "$compilerDirectory\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @includePaths "$PSScriptRoot\HardwareProbe.cpp" /link @libraryPaths d3d11.lib dxgi.lib /OUT:KoeHardwareProbe.dll
    if ($LASTEXITCODE -ne 0) {
        throw 'Hardware probe compilation failed'
    }
}
finally {
    Pop-Location
}

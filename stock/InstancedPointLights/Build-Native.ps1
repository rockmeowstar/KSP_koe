param([string]$OutputDirectory, [switch]$MapCaptureAudit, [switch]$CaptureLookupAudit)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot)
. "$projectRoot\core\BuildTools.ps1"
$buildTools = Get-KoeBuildTools
$compilerDirectory = $buildTools.Compiler
$windowsSdk = $buildTools.SDK
$sdkVersion = $buildTools.Version
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot 'artifacts\point-lights-production'
}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$includePaths = @("$compilerDirectory\include","$windowsSdk\Include\$sdkVersion\ucrt","$windowsSdk\Include\$sdkVersion\shared","$windowsSdk\Include\$sdkVersion\um","$windowsSdk\Include\$sdkVersion\winrt","$projectRoot\third_party\minhook\include",$PSScriptRoot) | ForEach-Object {
    "/I$_"
}
$libraryPaths = @("$compilerDirectory\lib\x64","$windowsSdk\Lib\$sdkVersion\ucrt\x64","$windowsSdk\Lib\$sdkVersion\um\x64") | ForEach-Object {
    "/LIBPATH:$_"
}
$minHookSources = @('buffer.c','hook.c','trampoline.c','hde\hde64.c') | ForEach-Object {
    Join-Path $projectRoot "third_party\minhook\src\$_"
}
Push-Location $OutputDirectory
$auditDefines = @(); if ($MapCaptureAudit) { $auditDefines += '/DKOE_MAP_CAPTURE_AUDIT' }
if ($CaptureLookupAudit) { $auditDefines += '/DKOE_CAPTURE_LOOKUP_AUDIT' }
try {
    & "$compilerDirectory\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @auditDefines @includePaths "$PSScriptRoot\NativeInstancedLights.cpp" "$PSScriptRoot\InstancedLightBatch.cpp" @minHookSources /link @libraryPaths d3d11.lib dxgi.lib bcrypt.lib /OUT:KoeInstancedLights.dll
    if ($LASTEXITCODE -ne 0) {
        throw 'Production point-light compilation failed'
    }
}
finally {
    Pop-Location
}

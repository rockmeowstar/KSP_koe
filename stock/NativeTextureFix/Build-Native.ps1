param([string]$OutputPath)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path (Split-Path $PSScriptRoot)
if (!$OutputPath) {
    $OutputPath = Join-Path $projectRoot 'artifacts\native-texture-production\NativeTextureEncoder.bin'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
. "$projectRoot\core\BuildTools.ps1"
$buildTools = Get-KoeBuildTools
$compilerDirectory = $buildTools.Compiler
$windowsSdk = $buildTools.SDK
$sdkVersion = $buildTools.Version
$buildDirectory = Split-Path $OutputPath
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
$previousPath = $env:PATH
try {
    $env:PATH = (Join-Path $compilerDirectory 'bin\Hostx64\x64')+';'+$previousPath
    $includePaths = @((Join-Path $compilerDirectory 'include'),(Join-Path $windowsSdk "Include\$sdkVersion\ucrt"),(Join-Path $windowsSdk "Include\$sdkVersion\shared"),(Join-Path $windowsSdk "Include\$sdkVersion\um"),(Join-Path $projectRoot 'third_party\minhook\include')) | ForEach-Object {
        "/I$_"
    }
    $libraryPaths = @((Join-Path $compilerDirectory 'lib\x64'),(Join-Path $windowsSdk "Lib\$sdkVersion\ucrt\x64"),(Join-Path $windowsSdk "Lib\$sdkVersion\um\x64")) | ForEach-Object {
        "/LIBPATH:$_"
    }
    $sourceFiles = @('buffer.c','hook.c','trampoline.c','hde\hde64.c') | ForEach-Object {
        Join-Path $projectRoot "third_party\minhook\src\$_"
    }
    Push-Location $buildDirectory
    try {
        & cl.exe /nologo /W4 /O2 /MT /LD /EHsc /std:c++17 @includePaths (Join-Path $PSScriptRoot 'NativeTextureEncoder.cpp') @sourceFiles /link @libraryPaths psapi.lib "/OUT:$OutputPath"
        if ($LASTEXITCODE -ne 0) {
            throw 'Native texture encoder build failed'
        }
    }
    finally {
        Pop-Location
    }
}
finally {
    $env:PATH = $previousPath
}

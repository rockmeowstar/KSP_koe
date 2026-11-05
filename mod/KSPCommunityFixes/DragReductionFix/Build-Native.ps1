param([string]$OutputDirectory = "$PSScriptRoot/native-assets")
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../../../core/BuildTools.ps1"
$tools = Get-KoeBuildTools
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$intermediate = "$PSScriptRoot/../../../artifacts/drag-reduction-build"
New-Item -ItemType Directory -Force $intermediate | Out-Null
$arguments = @('/nologo','/LD','/O2','/EHsc','/std:c++17','/MT',"/I$($tools.Compiler)/include")
foreach ($part in @('ucrt','shared','um','winrt')) {
    $arguments += "/I$($tools.SDK)/Include/$($tools.Version)/$part"
}
$arguments += @("$PSScriptRoot/DragReduction.cpp","/Fo$intermediate/DragReduction.obj",'/link',
    "/IMPLIB:$intermediate/KoeDragReduction.lib",
    "/OUT:$OutputDirectory/KoeDragReduction.dll","/LIBPATH:$($tools.Compiler)/lib/x64",
    "/LIBPATH:$($tools.SDK)/Lib/$($tools.Version)/ucrt/x64",
    "/LIBPATH:$($tools.SDK)/Lib/$($tools.Version)/um/x64",'d3d11.lib','dxgi.lib','d3dcompiler.lib')
& "$($tools.Compiler)/bin/Hostx64/x64/cl.exe" @arguments
if ($LASTEXITCODE -ne 0) { throw 'Drag reduction native build failed.' }

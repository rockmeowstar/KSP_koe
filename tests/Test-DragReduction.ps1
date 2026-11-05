$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
. "$root/core/BuildTools.ps1"
$tools = Get-KoeBuildTools
$output = "$root/artifacts/drag-reduction-tests"
New-Item -ItemType Directory -Force $output | Out-Null
$arguments = @('/nologo','/O2','/EHsc','/std:c++17','/MT',"/I$($tools.Compiler)/include")
foreach ($part in @('ucrt','shared','um','winrt')) {
    $arguments += "/I$($tools.SDK)/Include/$($tools.Version)/$part"
}
$arguments += @("$PSScriptRoot/DragReductionTests.cpp","/Fo$output/DragReductionTests.obj",'/link',
    "/OUT:$output/DragReductionTests.exe","/LIBPATH:$($tools.Compiler)/lib/x64",
    "/LIBPATH:$($tools.SDK)/Lib/$($tools.Version)/ucrt/x64",
    "/LIBPATH:$($tools.SDK)/Lib/$($tools.Version)/um/x64",'d3d11.lib','dxgi.lib','d3dcompiler.lib')
& "$($tools.Compiler)/bin/Hostx64/x64/cl.exe" @arguments
if ($LASTEXITCODE -ne 0) { throw 'GPU drag tests did not compile.' }
& "$output/DragReductionTests.exe"
if ($LASTEXITCODE -ne 0) { throw "GPU drag tests failed: $LASTEXITCODE" }

param([string]$ShaderCorpus,[string]$LegacyFlowSource,[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
. "$repo\core\BuildTools.ps1"
$tools=Get-KoeBuildTools
if(-not $OutputDirectory){$OutputDirectory=Join-Path $repo 'artifacts\frame-pipeline-tests'}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$includes=@("$($tools.Compiler)\include","$($tools.SDK)\Include\$($tools.Version)\ucrt","$($tools.SDK)\Include\$($tools.Version)\shared","$($tools.SDK)\Include\$($tools.Version)\um","$($tools.SDK)\Include\$($tools.Version)\winrt","$repo\third_party\nvidia-optical-flow-headers","$repo\stock\FrameGeneration","$repo\third_party\minhook\include")|ForEach-Object {"/I$_"}
$libs=@("$($tools.Compiler)\lib\x64","$($tools.SDK)\Lib\$($tools.Version)\ucrt\x64","$($tools.SDK)\Lib\$($tools.Version)\um\x64")|ForEach-Object {"/LIBPATH:$_"}
$hooks=@('buffer.c','hook.c','trampoline.c','hde\hde64.c')|ForEach-Object {Join-Path "$repo\third_party\minhook\src" $_}
$hash="$repo\third_party\DxbcHash\DxilHash.cpp"
$flow="$repo\stock\FrameGeneration\OpticalD3D11.cpp"
function Run-Test([string]$Name,[string[]]$Sources,[string[]]$Arguments=@()) {
    & "$($tools.Compiler)\bin\Hostx64\x64\cl.exe" /nologo /W4 /O2 /MT /EHsc /std:c++17 @includes @Sources /link @libs d3d11.lib dxgi.lib d3dcompiler.lib user32.lib bcrypt.lib "/OUT:$Name.exe"
    if($LASTEXITCODE -ne 0){throw "Compilation failed: $Name"}
    & (Join-Path $OutputDirectory "$Name.exe") @Arguments
    if($LASTEXITCODE -ne 0){throw "Test failed: $Name"}
}
Push-Location $OutputDirectory
try {
    Run-Test 'PresentationCadenceTests' @("$PSScriptRoot\PresentationCadenceTests.cpp")
    Run-Test 'FrameQueueTests' (@("$PSScriptRoot\FrameQueueTests.cpp",$flow,$hash)+$hooks)
    Run-Test 'IndependentLayerTests' @("$PSScriptRoot\IndependentLayerTests.cpp")
    Run-Test 'IndependentCaptureTests' (@("$PSScriptRoot\IndependentCaptureTests.cpp")+$hooks)
    $corpusArguments=@()
    if($ShaderCorpus){$corpusArguments+=((Resolve-Path -LiteralPath $ShaderCorpus).Path)}
    Run-Test 'EffectMrtTests' (@("$PSScriptRoot\EffectMrtTests.cpp",$hash)+$hooks) $corpusArguments
    if($LegacyFlowSource){
        Run-Test 'ConfidenceGateTests' @("$PSScriptRoot\ConfidenceGateTests.cpp",$flow) @((Resolve-Path -LiteralPath $LegacyFlowSource).Path)
    }
    Write-Output 'FRAME_PIPELINE_OFFLINE_TESTS_PASS (no KSP process launched)'
} finally {Pop-Location}

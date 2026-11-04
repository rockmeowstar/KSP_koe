param([Parameter(Mandatory=$true)][string]$GamePath)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot
$taskOutput=Join-Path $taskRoot 'artifacts\compatibility-tests'
New-Item -ItemType Directory -Force $taskOutput | Out-Null
$taskCsc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$taskManaged=Join-Path $GamePath 'KSP_x64_Data\Managed'
$taskReferences=@("/reference:$taskManaged\Assembly-CSharp.dll","/reference:$GamePath\GameData\000_Harmony\0Harmony.dll")
$taskReferences+=Get-ChildItem -LiteralPath $taskManaged -Filter 'UnityEngine*.dll' | ForEach-Object {"/reference:$($_.FullName)"}
function Test-Case([string]$Name,[string[]]$Sources,[string[]]$References=@(),[string[]]$Arguments=@()){
 & $taskCsc /nologo /optimize+ /unsafe "/out:$taskOutput\$Name.exe" @References "$taskRoot\core\Options\LoadingResources.cs" @Sources
 if($LASTEXITCODE -ne 0){throw "Compilation failed: $Name"}
 & "$taskOutput\$Name.exe" @Arguments
 if($LASTEXITCODE -ne 0){throw "Regression failed: $Name"}
}
Test-Case 'OptionStoreTests' @("$taskRoot\core\Options\OptionStore.cs","$PSScriptRoot\OptionStoreTests.cs")
$taskCloud=@(Get-ChildItem "$taskRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix" -Filter '*.cs' | ForEach-Object {$_.FullName})
Test-Case 'CloudLoopTests' ($taskCloud+@("$PSScriptRoot\OptionTestSettings.cs","$PSScriptRoot\CloudLoopTests.cs")) $taskReferences @($GamePath)
Test-Case 'EveTextureTests' @("$taskRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\EveTexturePipeline.cs","$taskRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\EarlyFileBank.cs","$PSScriptRoot\OptionTestSettings.cs","$PSScriptRoot\EveTextureTests.cs") $taskReferences @($GamePath)
$taskPqs=@(Get-ChildItem "$taskRoot\stock\PqsNoiseFix" -Filter '*.cs' | ForEach-Object {$_.FullName})
Test-Case 'PqsKernelTests' ($taskPqs+@("$PSScriptRoot\OptionTestSettings.cs","$PSScriptRoot\PqsKernelTests.cs")) $taskReferences @($GamePath)
Test-Case 'PqsNoiseCloneTests' @("$PSScriptRoot\PqsNoiseCloneTests.cs") $taskReferences @($GamePath)
Test-Case 'SwitchCombinationTests' @("$PSScriptRoot\SwitchCombinationTests.cs","$PSScriptRoot\OptionTestSettings.cs","$taskRoot\stock\DdsUploadFix\DdsUploadFix.cs","$taskRoot\stock\DdsUploadFix\Prefetch.cs","$taskRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\EveTexturePipeline.cs","$taskRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\EarlyFileBank.cs") $taskReferences @($GamePath)
Write-Output 'REGRESSION_PASS: config, original KSP/EVE IL, noise bit parity; no game launched'

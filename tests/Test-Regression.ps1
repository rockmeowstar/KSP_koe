param([Parameter(Mandatory = $true)][string]$GamePath)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'artifacts\compatibility-tests'
New-Item -ItemType Directory -Force $outputDirectory | Out-Null
$csharpCompiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$managedDirectory = Join-Path $GamePath 'KSP_x64_Data\Managed'
$compilerReferences = @("/reference:$managedDirectory\Assembly-CSharp.dll","/reference:$GamePath\GameData\000_Harmony\0Harmony.dll")
$compilerReferences += Get-ChildItem -LiteralPath $managedDirectory -Filter 'UnityEngine*.dll' | ForEach-Object {
    "/reference:$($_.FullName)"
}
function Test-Case([string]$Name,[string[]]$Sources,[string[]]$References = @(),[string[]]$Arguments = @()) {
    & $csharpCompiler /nologo /optimize+ /unsafe "/out:$outputDirectory\$Name.exe" @References "/reference:$managedDirectory\UnityEngine.CoreModule.dll" "$projectRoot\core\Options\ManagedContract.cs" "$projectRoot\core\Options\LoadingResources.cs" "$projectRoot\core\Options\LiveOptions.cs" @Sources
    if ($LASTEXITCODE -ne 0) {
        throw "Compilation failed: $Name"
    }
    & "$outputDirectory\$Name.exe" @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Regression failed: $Name"
    }
}
Test-Case 'OptionStoreTests' @("$projectRoot\core\Options\OptionStore.cs","$PSScriptRoot\OptionStoreTests.cs")
$cloudSources = @(Get-ChildItem "$projectRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix" -Filter '*.cs' | ForEach-Object {
        $_.FullName
    })
Test-Case 'CloudLoopTests' ($cloudSources+@("$PSScriptRoot\OptionTestSettings.cs","$PSScriptRoot\CloudLoopTests.cs")) $compilerReferences @($GamePath)
Test-Case 'EveTextureTests' @("$projectRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\EveTexturePipeline.cs","$projectRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\EarlyFileBank.cs","$PSScriptRoot\OptionTestSettings.cs","$PSScriptRoot\EveTextureTests.cs") $compilerReferences @($GamePath)
$pqsSources = @(Get-ChildItem "$projectRoot\stock\PqsNoiseFix" -Filter '*.cs' | ForEach-Object {
        $_.FullName
    })
Test-Case 'PqsKernelTests' ($pqsSources+@("$PSScriptRoot\OptionTestSettings.cs","$PSScriptRoot\PqsKernelTests.cs")) $compilerReferences @($GamePath)
Test-Case 'PqsNoiseCloneTests' @("$PSScriptRoot\PqsNoiseCloneTests.cs") $compilerReferences @($GamePath)
Test-Case 'SwitchCombinationTests' @("$PSScriptRoot\SwitchCombinationTests.cs","$PSScriptRoot\OptionTestSettings.cs","$projectRoot\stock\DdsUploadFix\DdsUploadFix.cs","$projectRoot\stock\DdsUploadFix\Prefetch.cs","$projectRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\EveTexturePipeline.cs","$projectRoot\mod\EnvironmentalVisualEnhancements\CloudLoopFix\EarlyFileBank.cs") $compilerReferences @($GamePath)
Write-Output 'Regression checks passed: options, KSP/EVE patches, noise parity and DDS combinations.'

param(
    [Parameter(Mandatory = $true)][string]$GamePath,
    [string]$OptionsAssembly = "$PSScriptRoot\..\dist\GameData\KerbalOverEngineering\Plugins\KerbalOverEngineering.Options.dll"
)
$ErrorActionPreference = 'Stop'
$outputDirectory = Join-Path $PSScriptRoot '..\artifacts\compatibility-tests'
New-Item -ItemType Directory -Force $outputDirectory | Out-Null
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$executable = Join-Path $outputDirectory 'SettingsCompatibilityTests.exe'
& $compiler /nologo /optimize+ "/out:$executable" "$PSScriptRoot\SettingsCompatibilityTests.cs"
if ($LASTEXITCODE -ne 0) { throw 'Settings compatibility tests did not compile' }
& $executable $OptionsAssembly (Join-Path $GamePath 'KSP_x64_Data\Managed')
if ($LASTEXITCODE -ne 0) { throw 'Settings compatibility tests failed' }

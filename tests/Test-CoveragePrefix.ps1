param([string]$NativeDirectory, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo 'artifacts/coverage-prefix-tests' }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$sources = @('CoverageZeroMap.cs','CoverageNative.cs') | ForEach-Object { "$repo\mod\EnvironmentalVisualEnhancements\CloudLoopFix\$_" }
& $csc /nologo /optimize+ /unsafe /platform:x64 "/out:$OutputDirectory\CoveragePrefixTests.exe" @sources "$PSScriptRoot\OptionTestSettings.cs" "$PSScriptRoot\CoveragePrefixTests.cs"
if ($LASTEXITCODE -ne 0) { throw 'Coverage prefix test compilation failed' }
& "$OutputDirectory/CoveragePrefixTests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Managed coverage / native fallback failed' }
if ($NativeDirectory) {
    & "$OutputDirectory/CoveragePrefixTests.exe" $NativeDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Rust coverage parity failed' }
}

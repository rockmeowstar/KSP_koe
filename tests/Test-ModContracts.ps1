param([Parameter(Mandatory=$true)][string]$GamePath)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'artifacts\mod-contract-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$unity = Join-Path $GamePath 'KSP_x64_Data\Managed\UnityEngine.CoreModule.dll'
& $csc /nologo /optimize+ "/reference:$unity" "/out:$out\ManagedContractTests.exe" "$repo\core\Options\ManagedContract.cs" "$PSScriptRoot\ManagedContractTests.cs"
if ($LASTEXITCODE -ne 0) { throw 'Mod contract test compilation failed' }
Copy-Item -LiteralPath $unity -Destination $out -Force
& "$out\ManagedContractTests.exe" $GamePath
if ($LASTEXITCODE -ne 0) { throw 'Mod contract compatibility checks failed' }

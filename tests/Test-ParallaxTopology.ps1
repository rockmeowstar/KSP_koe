param([Parameter(Mandatory=$true)][string]$GamePath,[string]$Rustc='rustc')
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
$out=Join-Path $repo 'artifacts/parallax-reuse-tests'
New-Item -ItemType Directory -Force $out | Out-Null
& $Rustc --edition=2021 --test -O "$repo/mod/ParallaxContinued/TopologyReuseFix/Topology.rs" -o "$out/TopologyTests.exe"
if($LASTEXITCODE){throw 'Topology test compilation failed'}
& "$out/TopologyTests.exe"
if($LASTEXITCODE){throw 'Topology tests failed'}
$csc="$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$unity="$GamePath\KSP_x64_Data\Managed\UnityEngine.CoreModule.dll"
& $csc /nologo /optimize+ "/r:$unity" "/out:$out\ContractTests.exe" "$repo\core\Options\ManagedContract.cs" "$repo\core\Options\ParallaxTopologyContract.cs" "$PSScriptRoot\ParallaxTopologyContractTests.cs"
if($LASTEXITCODE){throw 'Contract test compilation failed'}
Copy-Item -LiteralPath $unity -Destination $out -Force
& "$out/ContractTests.exe" $GamePath
if($LASTEXITCODE){throw 'Contract tests failed'}
& $csc /nologo /optimize+ "/out:$out\OptionStoreTests.exe" "$repo\core\Options\OptionStore.cs" "$PSScriptRoot\OptionStoreTests.cs"
if($LASTEXITCODE){throw 'Option test compilation failed'}
& "$out/OptionStoreTests.exe"
if($LASTEXITCODE){throw 'Option tests failed'}

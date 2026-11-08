$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$output = "$root\artifacts\dlc-cache-tests"
New-Item -ItemType Directory -Force $output | Out-Null
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $compiler /nologo /optimize+ "/out:$output\DlcCacheTests.exe" "$PSScriptRoot\DlcCacheTests.cs" "$root\stock\DlcCacheFix\CacheStore.cs" "$root\stock\DlcCacheFix\ModFingerprint.cs"
if ($LASTEXITCODE -ne 0) { throw 'DLC cache tests did not compile.' }
& "$output\DlcCacheTests.exe"
if ($LASTEXITCODE -ne 0) { throw 'DLC cache tests failed.' }
& $compiler /nologo /optimize+ "/out:$output\CacheGenerationTests.exe" "$PSScriptRoot\CacheGenerationTests.cs" "$root\mod\KSPCommunityFixes\EarlyDlcCacheFix\GenerationCache.cs"
if ($LASTEXITCODE -ne 0) { throw 'Cache retention tests did not compile.' }
& "$output\CacheGenerationTests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Cache generation retention failed.' }
& $compiler /nologo /optimize+ "/out:$output\OptionStoreTests.exe" "$PSScriptRoot\OptionStoreTests.cs" "$root\core\Options\OptionStore.cs"
if ($LASTEXITCODE -ne 0) { throw 'Option tests did not compile.' }
& "$output\OptionStoreTests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Option tests failed.' }

param([string]$GamePath = $env:KSP_ROOT)
$ErrorActionPreference = 'Stop'
if (!$GamePath) { throw 'Specify -GamePath or KSP_ROOT' }
$repo = Split-Path $PSScriptRoot
$output = Join-Path $repo 'artifacts\firefly-lifetime-tests'
New-Item -ItemType Directory -Force $output | Out-Null
$managed = Join-Path $GamePath 'KSP_x64_Data\Managed'
$references = @('/nologo','/optimize+','/unsafe','/target:exe',"/reference:$managed\Assembly-CSharp.dll")
$references += Get-ChildItem -LiteralPath $managed -Filter 'UnityEngine*.dll' | ForEach-Object { "/reference:$($_.FullName)" }
$dependencies = @("$GamePath\GameData\KerbalOverEngineering\Plugins\KerbalOverEngineering.Options.dll", "$GamePath\GameData\000_Harmony\0Harmony.dll", "$GamePath\GameData\Firefly\Plugins\Firefly.dll", "$GamePath\GameData\FireflyAPI\Plugins\FireflyAPI.dll")
$references += $dependencies | ForEach-Object { "/reference:$_" }
$sources = Get-ChildItem -LiteralPath "$repo\mod\Firefly\IdleFix" -Filter '*.cs' | ForEach-Object { $_.FullName }
$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $csc @references "/out:$output\FireflyLifetimeTests.exe" @sources "$PSScriptRoot\FireflyLifetimeTests.cs"
if ($LASTEXITCODE -ne 0) { throw 'Lifetime test compilation failed' }
Copy-Item -LiteralPath "$managed\Assembly-CSharp.dll" -Destination $output -Force
Get-ChildItem -LiteralPath $managed -Filter 'UnityEngine*.dll' | Copy-Item -Destination $output -Force
$dependencies | Copy-Item -Destination $output -Force
& "$output\FireflyLifetimeTests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Lifetime test failed' }

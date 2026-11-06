param([string]$Rustc='rustc', [string]$OutputDirectory, [switch]$UpdateAsset)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path (Split-Path $PSScriptRoot))
. "$repo/core/BuildTools.ps1"
$tools = Get-KoeBuildTools
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo 'artifacts/coverage-kernel-build' }
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$libs = @("$($tools.Compiler)/lib/x64", "$($tools.SDK)/Lib/$($tools.Version)/ucrt/x64", "$($tools.SDK)/Lib/$($tools.Version)/um/x64")
$linkArgs = $libs | ForEach-Object { '-L'; "native=$_" }
& $Rustc --edition=2021 --crate-name koe_coverage --crate-type cdylib --target x86_64-pc-windows-msvc -C opt-level=3 -C panic=abort -C target-cpu=x86-64 -C target-feature=+sse2,-avx,-avx2,-fma -C target-feature=+crt-static -C "linker=$($tools.Compiler)/bin/Hostx64/x64/link.exe" @linkArgs "$PSScriptRoot/CoverageKernel.rs" -o "$OutputDirectory/KoeCoverageKernel.dll"
if ($LASTEXITCODE -ne 0) { throw 'Coverage Rust kernel compilation failed' }
$record = [ordered]@{
    sourceSha256 = (Get-FileHash -LiteralPath "$PSScriptRoot/CoverageKernel.rs" -Algorithm SHA256).Hash.ToLowerInvariant()
    binarySha256 = (Get-FileHash -LiteralPath "$OutputDirectory/KoeCoverageKernel.dll" -Algorithm SHA256).Hash.ToLowerInvariant()
    compiler = (& $Rustc --version | Out-String).Trim()
}
[IO.File]::WriteAllText("$OutputDirectory/KoeCoverageKernel.build.json",($record | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
if ($UpdateAsset) {
    New-Item -ItemType Directory -Force "$PSScriptRoot/native-assets" | Out-Null
    Copy-Item -LiteralPath "$OutputDirectory/KoeCoverageKernel.dll","$OutputDirectory/KoeCoverageKernel.build.json" -Destination "$PSScriptRoot/native-assets"
}

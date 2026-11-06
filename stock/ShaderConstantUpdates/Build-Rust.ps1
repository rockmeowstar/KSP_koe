param([string]$Rustc='rustc', [string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot)
if(!$OutputDirectory){$OutputDirectory=Join-Path $repo 'artifacts\shader-constants-rust'}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$source=Join-Path $PSScriptRoot 'UniformUpdate.rs'
& $Rustc --version
if($LASTEXITCODE -ne 0){throw 'Rust compiler unavailable'}
& $Rustc --edition=2021 --crate-name koe_uniform_rust --crate-type lib --target x86_64-pc-windows-msvc -C opt-level=3 -C panic=abort -C overflow-checks=off -C target-cpu=x86-64 -C target-feature=+sse2,-avx,-avx2,-fma "--emit=obj=$OutputDirectory\RustKernel.obj,asm=$OutputDirectory\RustKernel.s" $source
if($LASTEXITCODE -ne 0){throw 'Rust kernel compilation failed'}
& python "$PSScriptRoot\Extract-RustKernel.py" "$OutputDirectory\RustKernel.obj" "$OutputDirectory\RustKernel.h" $source
if($LASTEXITCODE -ne 0){throw 'Rust kernel is not a supported relocation-free leaf'}
Copy-Item -LiteralPath "$OutputDirectory\RustKernel.h" -Destination "$PSScriptRoot\RustKernel.h"

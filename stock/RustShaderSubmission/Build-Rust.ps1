param([string]$Rustc='rustc', [string]$OutputDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot)
if(!$OutputDirectory){$OutputDirectory=Join-Path $repo 'artifacts\rust-shader-submission'}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
& $Rustc --edition=2021 --crate-name koe_shader_submission --crate-type staticlib --target x86_64-pc-windows-msvc -C opt-level=3 -C panic=abort -C target-cpu=x86-64 -C target-feature=+sse2,-avx,-avx2,-fma,+crt-static -C codegen-units=1 "$PSScriptRoot\Submission.rs" -o "$OutputDirectory\RustSubmission.lib"
if($LASTEXITCODE -ne 0){throw 'Rust shader submission compilation failed'}

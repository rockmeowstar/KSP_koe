param([string]$OutputDirectory,[string]$Rustc='rustc')
$ErrorActionPreference='Stop'
if(!$OutputDirectory){$OutputDirectory=Join-Path $PSScriptRoot 'native-assets'}
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
& $Rustc --edition=2021 --crate-type=cdylib -C opt-level=3 -C target-feature=+crt-static "$PSScriptRoot\Topology.rs" -o "$OutputDirectory\KoeParallaxTopology.dll"
if($LASTEXITCODE -ne 0){throw 'Parallax topology kernel compilation failed'}
$sources=@{}
foreach($name in @('Topology.rs','Certificate.rs')){$sources[$name]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name)).Hash.ToLowerInvariant()}
$manifest=@{binarySha256=(Get-FileHash -LiteralPath "$OutputDirectory\KoeParallaxTopology.dll").Hash.ToLowerInvariant();sources=$sources}
[IO.File]::WriteAllText("$OutputDirectory\KoeParallaxTopology.build.json",($manifest|ConvertTo-Json),[Text.UTF8Encoding]::new($false))

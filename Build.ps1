param([string]$GamePath='D:\SteamLibrary\steamapps\common\Kerbal Space Program')
$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
$taskCsc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$taskManaged=Join-Path $GamePath 'KSP_x64_Data\Managed'
$taskPackage=Join-Path $taskRoot 'dist\GameData\KerbalOverEngineering'
$taskBuild=Join-Path $taskRoot 'artifacts\package38-build'
New-Item -ItemType Directory -Force $taskPackage,$taskBuild | Out-Null
$taskCore=Join-Path $taskPackage 'Plugins\KerbalOverEngineering.Options.dll'
New-Item -ItemType Directory -Force (Split-Path $taskCore) | Out-Null
$taskReferences=@('/nologo','/optimize+','/unsafe','/target:library',"/reference:$taskManaged\Assembly-CSharp.dll")
$taskReferences+=Get-ChildItem -LiteralPath $taskManaged -Filter 'UnityEngine*.dll' | ForEach-Object {"/reference:$($_.FullName)"}
$taskCoreSources=Get-ChildItem -LiteralPath "$taskRoot\core\Options" -Filter '*.cs' | ForEach-Object {$_.FullName}
& "$taskRoot\core\Options\Build-HardwareProbe.ps1" -OutputDirectory "$taskBuild\hardware"
$taskProbe="$taskBuild\hardware\KoeHardwareProbe.dll"
[IO.File]::WriteAllText("$taskBuild\hardware\native.manifest",'KoeHardwareProbe.dll '+(Get-FileHash -LiteralPath $taskProbe).Hash,[Text.UTF8Encoding]::new($false))
& $taskCsc @taskReferences "/out:$taskCore" "/resource:$taskProbe,native.file.KoeHardwareProbe.dll" "/resource:$taskBuild\hardware\native.manifest,native.manifest" @taskCoreSources
if($LASTEXITCODE -ne 0){throw 'Options/core compilation failed'}
$taskReferences+="/reference:$taskCore"
$taskReferences+="/reference:$GamePath\GameData\000_Harmony\0Harmony.dll"
$taskReferences+="/reference:$GamePath\GameData\Firefly\Plugins\Firefly.dll"
$taskReferences+="/reference:$GamePath\GameData\FireflyAPI\Plugins\FireflyAPI.dll"

# Each payload is resolved only after its options/dependencies pass. Canonical wrapper
# paths overwrite previous module DLLs on an ordinary GameData copy/upgrade.
$taskModules=@(
 @{Path='stock/ExhaustSyncFix';Name='ExhaustSyncFix';Keys='Stock.ExhaustSync';Types='ExhaustSyncFix.Bootstrap'},
 @{Path='stock/PropellantRequestFix';Name='PropellantRequestFix';Keys='Stock.PropellantMessageCache,Stock.DeltaVLookup,Stock.SimulatedTransfer';Types='PropellantRequestFix.Bootstrap'},
 @{Path='stock/ExpansionBufferFix';Name='ExpansionBufferFix';Keys='Stock.ExpansionBufferReuse';Types='ExpansionBufferFix.Bootstrap'},
 @{Path='stock/DdsUploadFix';Name='DdsUploadFix';Keys='Stock.DdsDirectUpload,Stock.DdsPrefetch';Types='DdsUploadFix.Bootstrap'},
 @{Path='stock/CraftLinkFix';Name='CraftLinkFix';Keys='Stock.CraftLinkIndex';Types='CraftLinkFix.Bootstrap'},
 @{Path='stock/DlcCacheFix';Name='DlcCacheFix';Keys='Stock.DlcLz4Cache,Stock.DlcEarlyPreparation';Types="DlcCacheFix.Bootstrap`nDlcCacheFix.CacheBuilder"},
 @{Path='stock/PngPipelineFix';Name='PngPipelineFix';Keys='Stock.PngPipeline';Types='PngPipelineFix.Bootstrap'},
 @{Path='stock/LoadingScreenFix';Name='LoadingScreenFix';Keys='Stock.ReadySceneReveal';Types='LoadingScreenFix.Bootstrap'},
 @{Path='stock/DatabaseSchedulerFix';Name='DatabaseSchedulerFix';Keys='Stock.DatabaseChildWaitDrain,Stock.DatabaseReadyAsyncDrain,Stock.DatabaseFrameBudget';Types='DatabaseSchedulerFix.Bootstrap'},
 @{Path='stock/NativeTextureFix';Name='NativeTextureFix';Keys='Stock.NativeTextureEncoder';Types='NativeTextureFix.Bootstrap'},
 @{Path='stock/PqsNoiseFix';Name='PqsNoiseFix';Keys='Stock.PqsNoisePreparation,Stock.PqsPatchStateMemo';Types='PqsNoiseFix.Bootstrap'},
 @{Path='stock/PortraitFrameFix';Name='PortraitFrameFix';Keys='Stock.PortraitFullRate,Stock.PortraitHiddenCull';Types='KerbalOverEngineering.Portraits.PortraitFrameFix'},
 @{Path='mod/EnvironmentalVisualEnhancements/CloudLoopFix';Name='CloudLoopFix';Keys='EVE.IntervalGeometryMemo,EVE.LayerOrderMemo,EVE.MaterialSetupDedup,EVE.TexturePrefetch,EVE.DdsDirectUpload,EVE.MenuInitScheduler,EVE.CoverageZeroCull';Types="CloudLoopFix.Bootstrap`nCloudLoopFix.EveTextureBootstrap`nCloudLoopFix.MenuCloudScheduler`nCloudLoopFix.CoverageCull"},
 @{Path='mod/EnvironmentalVisualEnhancements/CurveBakeFix';Name='EveCurveBakeFix';Keys='EVE.CurveBakeHoist';Types='EveCurveBakeFix.Bootstrap'},
 @{Path='mod/ParallaxContinued/MenuSkyboxFix';Name='MenuSkyboxFix';Keys='ParallaxContinued.GpuSkyboxCopy,ParallaxContinued.DdsDirectUpload,ParallaxContinued.BiomeRgbPreparation,ParallaxContinued.LaunchTextureRetention';Types="MenuSkyboxFix.Bootstrap`nMenuSkyboxFix.DdsUploadBootstrap`nMenuSkyboxFix.LaunchBiomePipeline`nMenuSkyboxFix.LaunchTextureRetention"},
 @{Path='mod/Firefly/IdleFix';Name='FireflyIdleFix';Keys='Firefly.ZeroStrengthGate,Firefly.DepthLighting,Firefly.GlobalCommandDedup';Types='FireflyIdleFix.Bootstrap'},
 @{Path='mod/EnvironmentalVisualEnhancements/ShaderCorrectionGuards';Name='KoeCloudGuards';Keys='EVE.ShaderCorrectionGuards';Types='KerbalOverEngineering.CloudGuards.Bootstrap';Native='Cloud'},
 @{Path='stock/InstancedPointLights';Name='KoeInstancedPointLights';Keys='Rendering.InstancedPointLights';Types='KerbalOverEngineering.InstancedPointLights.Bootstrap';Native='Lights'},
 @{Path='stock/FrameGeneration';Name='KoeFrameGenerationBridge';Keys='Rendering.FrameGeneration';Types='KoeFrameGeneration.Bridge';Native='Frame'}
)
$taskNativeFiles=@{
 Cloud=@(@('KoeCloudGuard.dll','616f289df2a26ea15dbccdc8c6275ad76ca77805c96ae4661a2216b536cb2ccd'),@('base-guarded.koepatch','544be245373e4a7bc0dfa0df0a8254d4fb170738c61dcfc361afacb30bed9bee'),@('top-guarded.koepatch','97db0080cb6c833a317e746b465618a96ffb30f88ad80f31d28b649e80365131'))
 Lights=@(@('KSP_x64_Data/Plugins/KoeInstancedLights.dll','78f5ea8fd67cacfa5dd75241a542b5c4a3d71e581eb604333532cce17170193b'),@('GameData/KerbalOverEngineeringExperiments/InstancedLights/light0-ps.shdr','52f0befe1d3fa291a4bebef94502ef14c162f4a3d86cd579a87da9664249bef7'),@('GameData/KerbalOverEngineeringExperiments/InstancedLights/light0-vs.shdr','18f304a7ebaf6499cde707116f7cda855078b930ae59b750ed6b5b8a0c46e496'),@('GameData/KerbalOverEngineeringExperiments/InstancedLights/light1-ps.shdr','bddc54bcdf16b256e3fdc3eabf6ea7675717597293d40bbfc2deb397878e81c8'),@('GameData/KerbalOverEngineeringExperiments/InstancedLights/light1-vs.shdr','18f304a7ebaf6499cde707116f7cda855078b930ae59b750ed6b5b8a0c46e496'))
 Frame=,@('KSP_x64_Data/Plugins/KoeFrameGeneration.dll','e2d9ab5265e19de95e410b0c1a67e8261df6dead8601cea9ff3a71e869dc96a5')
}
foreach($taskModule in $taskModules){
 $taskName=$taskModule.Name
 $taskDirectory=Join-Path $taskBuild $taskName
 New-Item -ItemType Directory -Force $taskDirectory | Out-Null
 $taskPayload=Join-Path $taskDirectory "$taskName.Runtime.dll"
 $taskSources=@(Get-ChildItem -LiteralPath (Join-Path $taskRoot $taskModule.Path) -Filter '*.cs' | ForEach-Object {$_.FullName})
 $taskResources=@()
 if($taskName -eq 'NativeTextureFix'){$taskResources+="/resource:$taskRoot\artifacts\native-texture-production\NativeTextureEncoder.bin,encoder.bin"}
 if($taskModule.Native){
  $taskManifest=@()
  foreach($taskEntry in $taskNativeFiles[$taskModule.Native]){
   $taskAsset=Join-Path $taskRoot ($taskModule.Path+'/native-assets/'+(Split-Path $taskEntry[0] -Leaf))
   $taskHash=(Get-FileHash -LiteralPath $taskAsset -Algorithm SHA256).Hash.ToLowerInvariant()
   if($taskEntry[1] -and $taskHash -ne $taskEntry[1]){throw "Accepted native identity changed: $taskAsset"}
   $taskLeaf=Split-Path $taskAsset -Leaf
   $taskResources+="/resource:$taskAsset,native.file.$taskLeaf"
   $taskManifest+="$taskLeaf $taskHash"
  }
  $taskManifestPath=Join-Path $taskDirectory 'native.manifest'
  [IO.File]::WriteAllLines($taskManifestPath,$taskManifest,[Text.UTF8Encoding]::new($false))
  $taskResources+="/resource:$taskManifestPath,native.manifest"
 }
 & $taskCsc @taskReferences /define:KOE_COVERAGE_PRODUCTION "/out:$taskPayload" @taskResources @taskSources
 if($LASTEXITCODE -ne 0){throw "Payload build failed: $taskName"}
 $taskInfo=Join-Path $taskDirectory 'module.info'
 [IO.File]::WriteAllText($taskInfo,$taskModule.Keys+"`n"+$taskModule.Types,[Text.UTF8Encoding]::new($false))
 $taskWrapper=Join-Path $taskDirectory 'Bootstrap.cs'
 Copy-Item -LiteralPath "$taskRoot\core\Options\ModuleBootstrap.cs.template" -Destination $taskWrapper
 $taskOutput=Join-Path $taskPackage ($taskModule.Path+"/Plugins/$taskName.dll")
 New-Item -ItemType Directory -Force (Split-Path $taskOutput) | Out-Null
 & $taskCsc @taskReferences "/out:$taskOutput" "/resource:$taskPayload,module.bin" "/resource:$taskInfo,module.info" $taskWrapper
 if($LASTEXITCODE -ne 0){throw "Wrapper build failed: $taskName"}
}
Copy-Item -LiteralPath "$taskRoot\LICENSE","$taskRoot\LICENSES.md" -Destination $taskPackage
Copy-Item -LiteralPath "$taskRoot\licenses" -Destination $taskPackage -Recurse
Compress-Archive -LiteralPath "$taskRoot\dist\GameData" -DestinationPath "$taskRoot\dist\KerbalOverEngineering-0.15.0.zip" -CompressionLevel Optimal -Force
Write-Output 'BUILD_PASS: core + 19 modules; GameData ZIP; no installer'
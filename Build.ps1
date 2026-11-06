param([string]$GamePath = $env:KSP_ROOT,[string]$ReleaseVersion = '0.15.5')
$ErrorActionPreference = 'Stop'
if ($ReleaseVersion -notmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?$') {
    throw 'Invalid release version'
}
if (-not $GamePath -or -not (Test-Path -LiteralPath (Join-Path $GamePath 'KSP_x64.exe'))) {
    throw 'Specify -GamePath pointing to KSP, or set KSP_ROOT'
}
$projectRoot = $PSScriptRoot
$csharpCompiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$managedDirectory = Join-Path $GamePath 'KSP_x64_Data\Managed'
$packageDirectory = Join-Path $projectRoot 'dist\GameData\KerbalOverEngineering'
$buildDirectory = Join-Path $projectRoot 'artifacts\package38-build'
New-Item -ItemType Directory -Force $packageDirectory,$buildDirectory | Out-Null
$optionsAssembly = Join-Path $packageDirectory 'Plugins\KerbalOverEngineering.Options.dll'
New-Item -ItemType Directory -Force (Split-Path $optionsAssembly) | Out-Null
$compilerReferences = @('/nologo','/optimize+','/unsafe','/target:library',"/reference:$managedDirectory\Assembly-CSharp.dll")
$compilerReferences += Get-ChildItem -LiteralPath $managedDirectory -Filter 'UnityEngine*.dll' | ForEach-Object {
    "/reference:$($_.FullName)"
}
$optionsSources = Get-ChildItem -LiteralPath "$projectRoot\core\Options" -Filter '*.cs' | ForEach-Object {
    $_.FullName
}
& "$projectRoot\core\Options\Build-HardwareProbe.ps1" -OutputDirectory "$buildDirectory\hardware"
$hardwareProbe = "$buildDirectory\hardware\KoeHardwareProbe.dll"
[IO.File]::WriteAllText("$buildDirectory\hardware\native.manifest",'KoeHardwareProbe.dll '+(Get-FileHash -LiteralPath $hardwareProbe).Hash,[Text.UTF8Encoding]::new($false))
& $csharpCompiler @compilerReferences "/out:$optionsAssembly" "/resource:$hardwareProbe,native.file.KoeHardwareProbe.dll" "/resource:$buildDirectory\hardware\native.manifest,native.manifest" @optionsSources
if ($LASTEXITCODE -ne 0) {
    throw 'Options/core compilation failed'
}
$compilerReferences += "/reference:$optionsAssembly"
$compilerReferences += "/reference:$GamePath\GameData\000_Harmony\0Harmony.dll"
$compilerReferences += "/reference:$GamePath\GameData\Firefly\Plugins\Firefly.dll"
$compilerReferences += "/reference:$GamePath\GameData\FireflyAPI\Plugins\FireflyAPI.dll"
# Each payload is resolved only after its options/dependencies pass. Canonical wrapper
# paths overwrite previous module DLLs on an ordinary GameData copy/upgrade.
$modules = @(
    @{
        Path = 'stock/ExhaustSyncFix'
        Name = 'ExhaustSyncFix'
        Keys = 'Stock.ExhaustSync'
        Types = 'ExhaustSyncFix.Bootstrap'
    },
    @{
        Path = 'stock/PropellantRequestFix'
        Name = 'PropellantRequestFix'
        Keys = 'Stock.PropellantMessageCache,Stock.PropellantArgumentReuse,Stock.DeltaVLookup,Stock.SimulatedTransfer'
        Types = 'PropellantRequestFix.Bootstrap'
    },
    @{
        Path = 'stock/ExpansionBufferFix'
        Name = 'ExpansionBufferFix'
        Keys = 'Stock.ExpansionBufferReuse'
        Types = 'ExpansionBufferFix.Bootstrap'
    },
    @{
        Path = 'stock/DdsUploadFix'
        Name = 'DdsUploadFix'
        Keys = 'Stock.DdsDirectUpload,Stock.DdsPrefetch'
        Types = 'DdsUploadFix.Bootstrap'
    },
    @{
        Path = 'stock/CraftLinkFix'
        Name = 'CraftLinkFix'
        Keys = 'Stock.CraftLinkIndex'
        Types = 'CraftLinkFix.Bootstrap'
    },
    @{
        Path = 'stock/DlcCacheFix'
        Name = 'DlcCacheFix'
        Keys = 'Stock.DlcLz4Cache,Stock.DlcEarlyPreparation'
        Types = "DlcCacheFix.Bootstrap`nDlcCacheFix.CacheBuilder"
    },
    @{
        Path = 'stock/PngPipelineFix'
        Name = 'PngPipelineFix'
        Keys = 'Stock.PngPipeline'
        Types = 'PngPipelineFix.Bootstrap'
    },
    @{
        Path = 'stock/LoadingScreenFix'
        Name = 'LoadingScreenFix'
        Keys = 'Stock.ReadySceneReveal'
        Types = 'LoadingScreenFix.Bootstrap'
    },
    @{
        Path = 'stock/DatabaseSchedulerFix'
        Name = 'DatabaseSchedulerFix'
        Keys = 'Stock.DatabaseChildWaitDrain,Stock.DatabaseReadyAsyncDrain,Stock.DatabaseFrameBudget'
        Types = 'DatabaseSchedulerFix.Bootstrap'
    },
    @{
        Path = 'stock/NativeTextureFix'
        Name = 'NativeTextureFix'
        Keys = 'Stock.NativeTextureEncoder'
        Types = 'NativeTextureFix.Bootstrap'
    },
    @{
        Path = 'stock/PqsNoiseFix'
        Name = 'PqsNoiseFix'
        Keys = 'Stock.PqsNoisePreparation,Stock.PqsPatchStateMemo'
        Types = 'PqsNoiseFix.Bootstrap'
    },
    @{
        Path = 'stock/PortraitFrameFix'
        Name = 'PortraitFrameFix'
        Keys = 'Stock.PortraitFullRate,Stock.PortraitHiddenCull'
        Types = 'KerbalOverEngineering.Portraits.PortraitFrameFix'
    },
    @{
        Path = 'stock/EventPathFix'
        Name = 'KoeEventPathFix'
        Keys = 'Stock.StageListReuse,Stock.EvaTriggerListReuse,Stock.PropellantGaugeLookup'
        Types = 'KoeEventPathFix.Bootstrap'
    },
    @{
        Path = 'stock/DeltaVReadBatchFix'
        Name = 'KoeDeltaVReadBatchFix'
        Keys = 'Stock.DeltaVReadBatch'
        Types = 'KoeDeltaVReadBatchFix.Bootstrap'
    },
    @{
        Path = 'mod/FerramAerospaceResearch/EventPathFix'
        Name = 'KoeFarEventPath'
        Keys = 'FAR.RendererInitDispatch,FAR.HitSortBuffers,FAR.FairingLifecycle,FAR.ColliderOwnerIndex'
        Types = 'KoeFarEventPath.Bootstrap'
    },
    @{
        Path = 'mod/FreeIva/RefreshFix'
        Name = 'KoeFreeIvaRefresh'
        Keys = 'FreeIva.HatchConnectionDedup,FreeIva.ForeignVesselRefresh,FreeIva.RendererVisitBuffers'
        Types = 'KoeFreeIvaRefresh.Bootstrap'
    },
    @{
        Path = 'mod/KSPCommunityFixes/CollisionStateFix'
        Name = 'KoeCollisionStateFix'
        Keys = 'KSPCommunityFixes.CollisionStateDedup'
        Types = 'KoeCollisionStateFix.Bootstrap'
    },
    @{
        Path = 'mod/Waterfall/RendererIndexFix'
        Name = 'KoeWaterfallRendererIndex'
        Keys = 'Waterfall.RendererMembershipIndex'
        Types = 'KoeWaterfallRendererIndex.Bootstrap'
    },
    @{
        Path = 'mod/KSPCommunityFixes/DragReductionFix'
        Name = 'KoeDragReduction'
        Keys = 'KSPCommunityFixes.DragGpuReduction'
        Types = 'KoeDragReduction.Bootstrap'
        Native = 'Drag'
    },
    @{
        Path = 'mod/KSPCommunityFixes/EarlyDlcCacheFix'
        Name = 'KoeCfDlcCache'
        Keys = 'KSPCommunityFixes.EarlyDlcCache'
        Types = 'KoeCfDlcCache.Bootstrap'
    },
    @{
        Path = 'mod/EnvironmentalVisualEnhancements/CloudLoopFix'
        Name = 'CloudLoopFix'
        Native = 'Coverage'
        Keys = 'EVE.IntervalGeometryMemo,EVE.LayerOrderMemo,EVE.MaterialSetupDedup,EVE.TexturePrefetch,EVE.DdsDirectUpload,EVE.MenuInitScheduler,EVE.CoverageZeroCull'
        Types = "CloudLoopFix.Bootstrap`nCloudLoopFix.EveTextureBootstrap`nCloudLoopFix.MenuCloudScheduler`nCloudLoopFix.CoverageCull"
    },
    @{
        Path = 'mod/EnvironmentalVisualEnhancements/CurveBakeFix'
        Name = 'EveCurveBakeFix'
        Keys = 'EVE.CurveBakeHoist'
        Types = 'EveCurveBakeFix.Bootstrap'
    },
    @{
        Path = 'mod/ParallaxContinued/MenuSkyboxFix'
        Name = 'MenuSkyboxFix'
        Keys = 'ParallaxContinued.GpuSkyboxCopy,ParallaxContinued.DdsDirectUpload,ParallaxContinued.BiomeRgbPreparation,ParallaxContinued.LaunchTextureRetention'
        Types = "MenuSkyboxFix.Bootstrap`nMenuSkyboxFix.DdsUploadBootstrap`nMenuSkyboxFix.LaunchBiomePipeline`nMenuSkyboxFix.LaunchTextureRetention"
    },
    @{
        Path = 'mod/Firefly/IdleFix'
        Name = 'FireflyIdleFix'
        Keys = 'Firefly.ZeroStrengthGate,Firefly.DepthLighting,Firefly.GlobalCommandDedup'
        Types = 'FireflyIdleFix.Bootstrap'
    },
    @{
        Path = 'mod/EnvironmentalVisualEnhancements/ShaderCorrectionGuards'
        Name = 'KoeCloudGuards'
        Keys = 'EVE.ShaderCorrectionGuards'
        Types = 'KerbalOverEngineering.CloudGuards.Bootstrap'
        Native = 'Cloud'
    },
    @{
        Path = 'stock/InstancedPointLights'
        Name = 'KoeInstancedPointLights'
        Keys = 'Rendering.InstancedPointLights'
        Types = 'KerbalOverEngineering.InstancedPointLights.Bootstrap'
        Native = 'Lights'
    },
    @{
        Path = 'stock/FrameGeneration'
        Name = 'KoeFrameGenerationBridge'
        Keys = 'Rendering.FrameGeneration,Rendering.AdaptiveFrameCadence'
        Types = 'KoeFrameGeneration.Bridge'
        Native = 'Frame'
    },
    @{
        Path = 'stock/ShaderConstantUpdates'
        Name = 'KoeShaderConstantUpdates'
        Keys = 'Rendering.ShaderConstantUpdates'
        Types = 'KerbalOverEngineering.ShaderConstants.Bootstrap'
        Native = 'Uniforms'
    }
)
& "$projectRoot/mod/KSPCommunityFixes/DragReductionFix/Build-Native.ps1"
& "$projectRoot/stock/ShaderConstantUpdates/Build-Native.ps1"
$coveragePath = "$projectRoot/mod/EnvironmentalVisualEnhancements/CloudLoopFix"
$coverageBuild = Get-Content -LiteralPath "$coveragePath/native-assets/KoeCoverageKernel.build.json" -Raw | ConvertFrom-Json
if ((Get-FileHash -LiteralPath "$coveragePath/CoverageKernel.rs" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $coverageBuild.sourceSha256) {
    throw 'Coverage Rust source changed; rebuild and validate the native coverage asset'
}
$nativeFiles = @{
    Coverage = ,@('KoeCoverageKernel.dll',$coverageBuild.binarySha256)
    Uniforms = ,@('KoeUniformKernel.dll',$null)
    Drag = ,@('KoeDragReduction.dll',$null)
    Cloud = @(@('KoeCloudGuard.dll','616f289df2a26ea15dbccdc8c6275ad76ca77805c96ae4661a2216b536cb2ccd'),@('base-guarded.koepatch','544be245373e4a7bc0dfa0df0a8254d4fb170738c61dcfc361afacb30bed9bee'),@('top-guarded.koepatch','97db0080cb6c833a317e746b465618a96ffb30f88ad80f31d28b649e80365131'))
    Lights = @(@('KSP_x64_Data/Plugins/KoeInstancedLights.dll','75d232aae1885345270bbc979c642e83b3fe3a4649ad17daf99ac7ae7ca51bf4'),@('GameData/KerbalOverEngineeringExperiments/InstancedLights/light0-ps.shdr','52f0befe1d3fa291a4bebef94502ef14c162f4a3d86cd579a87da9664249bef7'),@('GameData/KerbalOverEngineeringExperiments/InstancedLights/light0-vs.shdr','18f304a7ebaf6499cde707116f7cda855078b930ae59b750ed6b5b8a0c46e496'),@('GameData/KerbalOverEngineeringExperiments/InstancedLights/light1-ps.shdr','bddc54bcdf16b256e3fdc3eabf6ea7675717597293d40bbfc2deb397878e81c8'),@('GameData/KerbalOverEngineeringExperiments/InstancedLights/light1-vs.shdr','18f304a7ebaf6499cde707116f7cda855078b930ae59b750ed6b5b8a0c46e496'))
    Frame = ,@('KSP_x64_Data/Plugins/KoeFrameGeneration.dll','732fbe935aaef00f1fa4debcec73ea37b45b74921402c7d546c3e520f28155af')
}
foreach ($module in $modules) {
    $moduleName = $module.Name
    $moduleDirectory = Join-Path $buildDirectory $moduleName
    New-Item -ItemType Directory -Force $moduleDirectory | Out-Null
    $payloadAssembly = Join-Path $moduleDirectory "$moduleName.Runtime.dll"
    $sourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $projectRoot $module.Path) -Filter '*.cs' | ForEach-Object {
            $_.FullName
        })
    $embeddedResources = @()
    if ($moduleName -eq 'KoeCfDlcCache') {
        $sourceFiles += @("$projectRoot\stock\DlcCacheFix\CacheStore.cs", "$projectRoot\stock\DlcCacheFix\ModFingerprint.cs")
    }
    if ($moduleName -eq 'NativeTextureFix') {
        $embeddedResources += "/resource:$projectRoot\artifacts\native-texture-production\NativeTextureEncoder.bin,encoder.bin"
    }
    if ($module.Native) {
        $nativeManifest = @()
        foreach ($nativeEntry in $nativeFiles[$module.Native]) {
            $nativeAsset = Join-Path $projectRoot ($module.Path+'/native-assets/'+(Split-Path $nativeEntry[0] -Leaf))
            $assetHash = (Get-FileHash -LiteralPath $nativeAsset -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($nativeEntry[1] -and $assetHash -ne $nativeEntry[1]) {
                throw "Accepted native identity changed: $nativeAsset"
            }
            $assetName = Split-Path $nativeAsset -Leaf
            $embeddedResources += "/resource:$nativeAsset,native.file.$assetName"
            $nativeManifest += "$assetName $assetHash"
        }
        $manifestPath = Join-Path $moduleDirectory 'native.manifest'
        [IO.File]::WriteAllLines($manifestPath,$nativeManifest,[Text.UTF8Encoding]::new($false))
        $embeddedResources += "/resource:$manifestPath,native.manifest"
    }
    & $csharpCompiler @compilerReferences /define:KOE_COVERAGE_PRODUCTION "/out:$payloadAssembly" @embeddedResources @sourceFiles
    if ($LASTEXITCODE -ne 0) {
        throw "Payload build failed: $moduleName"
    }
    $moduleInfo = Join-Path $moduleDirectory 'module.info'
    [IO.File]::WriteAllText($moduleInfo,$module.Keys+"`n"+$module.Types,[Text.UTF8Encoding]::new($false))
    $moduleWrapper = Join-Path $moduleDirectory 'Bootstrap.cs'
    Copy-Item -LiteralPath "$projectRoot\core\Options\ModuleBootstrap.cs.template" -Destination $moduleWrapper
    $outputDirectory = Join-Path $packageDirectory ($module.Path+"/Plugins/$moduleName.dll")
    New-Item -ItemType Directory -Force (Split-Path $outputDirectory) | Out-Null
    & $csharpCompiler @compilerReferences "/out:$outputDirectory" "/resource:$payloadAssembly,module.bin" "/resource:$moduleInfo,module.info" $moduleWrapper
    if ($LASTEXITCODE -ne 0) {
        throw "Wrapper build failed: $moduleName"
    }
}
Copy-Item -LiteralPath "$projectRoot\LICENSE","$projectRoot\LICENSES.md" -Destination $packageDirectory
New-Item -ItemType Directory -Force "$packageDirectory\licenses" | Out-Null
Copy-Item -Path "$projectRoot\licenses\*" -Destination "$packageDirectory\licenses" -Force
Copy-Item -LiteralPath "$projectRoot\third_party\reshade-api\LICENSE" -Destination "$packageDirectory\licenses\ReShade-API.txt" -Force
. "$projectRoot\core\Packaging.ps1"
Write-KoeZip -SourceDirectory "$projectRoot\dist" -Destination "$projectRoot\dist\KerbalOverEngineering-$ReleaseVersion.zip" -ExcludePaths @('GameData/KerbalOverEngineering/Options.cfg')
Write-KoeZip -SourceDirectory $projectRoot -Destination "$projectRoot\dist\KerbalOverEngineering-$ReleaseVersion-source.zip" -Paths @(
    'core'
    'stock'
    'mod'
    'third_party'
    'tests'
    'licenses'
    'LICENSE'
    'LICENSES.md'
    'Build.ps1'
    'Build-PointLights.ps1'
    'artifacts/native-texture-production/NativeTextureEncoder.bin'
    '.gitignore'
    '.editorconfig'
    '.clang-format'
)
Write-Output "Build complete: core and $($modules.Count) modules; GameData and source ZIPs created."

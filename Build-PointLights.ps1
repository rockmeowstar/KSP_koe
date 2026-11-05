param([Parameter(Mandatory = $true)][string]$GamePath,[Parameter(Mandatory = $true)][string]$Assembler)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force "$outputDirectory\shaders" | Out-Null
$expectedShaderHashes = @{
    'light0-vs' = '18f304a7ebaf6499cde707116f7cda855078b930ae59b750ed6b5b8a0c46e496'
    'light1-vs' = '18f304a7ebaf6499cde707116f7cda855078b930ae59b750ed6b5b8a0c46e496'
    'light0-ps' = '52f0befe1d3fa291a4bebef94502ef14c162f4a3d86cd579a87da9664249bef7'
    'light1-ps' = 'bddc54bcdf16b256e3fdc3eabf6ea7675717597293d40bbfc2deb397878e81c8'
}
foreach ($shaderName in $expectedShaderHashes.Keys) {
    $shaderAssemblySource = Join-Path "$outputDirectory\shaders" ($shaderName+'.asm')
    Copy-Item -LiteralPath "$projectRoot\shader-sources\$shaderName.asm" -Destination $shaderAssemblySource
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $assemblerMessages = & $Assembler -a --copy-reflection "$projectRoot\shader-sources\$shaderName.reflection.dxbc" $shaderAssemblySource 2>&1
        $assemblerExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($assemblerExitCode -ne 0) {
        throw "Shader assembler failed: $shaderName; $assemblerMessages"
    }
    $compiledShader = Join-Path "$outputDirectory\shaders" ($shaderName+'.shdr')
    if ((Get-FileHash -LiteralPath $compiledShader).Hash.ToLowerInvariant() -ne $expectedShaderHashes[$shaderName]) {
        throw "Shader bytes differ: $shaderName"
    }
}
& "$projectRoot\stock\InstancedPointLights\Build-Native.ps1" -OutputDirectory "$outputDirectory\native"
& "$projectRoot\core\Options\Build-HardwareProbe.ps1" -OutputDirectory "$outputDirectory\probe"
$csharpCompiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$managedDirectory = Join-Path $GamePath 'KSP_x64_Data\Managed'
$compilerReferences = @('/nologo','/optimize+','/target:library',"/reference:$managedDirectory\Assembly-CSharp.dll","/reference:$managedDirectory\UnityEngine.CoreModule.dll")
$optionsAssembly = "$outputDirectory\KerbalOverEngineering.Options.dll"
$hardwareProbe = "$outputDirectory\probe\KoeHardwareProbe.dll"
[IO.File]::WriteAllText("$outputDirectory\probe.manifest",'KoeHardwareProbe.dll '+(Get-FileHash -LiteralPath $hardwareProbe).Hash)
$sourceFiles = @(Get-ChildItem -LiteralPath "$projectRoot\core\Options" -Filter '*.cs' | ForEach-Object {
        $_.FullName
    })
& $csharpCompiler @compilerReferences "/out:$optionsAssembly" "/resource:$hardwareProbe,native.file.KoeHardwareProbe.dll" "/resource:$outputDirectory\probe.manifest,native.manifest" @sourceFiles
if ($LASTEXITCODE -ne 0) {
    throw 'Core build failed'
}
$embeddedResources = @()
$nativeManifest = @()
foreach ($sourceFile in @((Get-Item "$outputDirectory\native\KoeInstancedLights.dll"))+(Get-ChildItem -LiteralPath "$outputDirectory\shaders" -Filter '*.shdr')) {
    $embeddedResources += "/resource:$($sourceFile.FullName),native.file.$($sourceFile.Name)"
    $nativeManifest += ($sourceFile.Name+' '+(Get-FileHash -LiteralPath $sourceFile.FullName).Hash)
}
[IO.File]::WriteAllLines("$outputDirectory\native.manifest",$nativeManifest)
$embeddedResources += "/resource:$outputDirectory\native.manifest,native.manifest"
$payloadAssembly = "$outputDirectory\KoeInstancedPointLights.Runtime.dll"
& $csharpCompiler @compilerReferences "/reference:$optionsAssembly" "/out:$payloadAssembly" @embeddedResources "$projectRoot\stock\InstancedPointLights\InstancedPointLights.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Payload build failed'
}
[IO.File]::WriteAllText("$outputDirectory\module.info","Rendering.InstancedPointLights`nKerbalOverEngineering.InstancedPointLights.Bootstrap")
Copy-Item -LiteralPath "$projectRoot\core\Options\ModuleBootstrap.cs.template" -Destination "$outputDirectory\Bootstrap.cs"
& $csharpCompiler @compilerReferences "/reference:$optionsAssembly" "/out:$outputDirectory\KoeInstancedPointLights.dll" "/resource:$payloadAssembly,module.bin" "/resource:$outputDirectory\module.info,module.info" "$outputDirectory\Bootstrap.cs"
if ($LASTEXITCODE -ne 0) {
    throw 'Wrapper build failed'
}
Write-Output 'CORRESPONDING_SOURCE_BUILD_PASS: exact four shader bytes + native renderer + core + managed payload/wrapper'

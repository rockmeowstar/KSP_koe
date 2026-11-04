param([Parameter(Mandatory=$true)][string]$GamePath,[Parameter(Mandatory=$true)][string]$Assembler)
$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
$taskOutput=Join-Path $taskRoot 'build'
New-Item -ItemType Directory -Force "$taskOutput\shaders" | Out-Null
$taskExpected=@{
 'light0-vs'='18f304a7ebaf6499cde707116f7cda855078b930ae59b750ed6b5b8a0c46e496'
 'light1-vs'='18f304a7ebaf6499cde707116f7cda855078b930ae59b750ed6b5b8a0c46e496'
 'light0-ps'='52f0befe1d3fa291a4bebef94502ef14c162f4a3d86cd579a87da9664249bef7'
 'light1-ps'='bddc54bcdf16b256e3fdc3eabf6ea7675717597293d40bbfc2deb397878e81c8'
}
foreach($taskName in $taskExpected.Keys){
 $taskAssembly=Join-Path "$taskOutput\shaders" ($taskName+'.asm')
 Copy-Item -LiteralPath "$taskRoot\shader-sources\$taskName.asm" -Destination $taskAssembly
 $taskPreference=$ErrorActionPreference
 try{
  $ErrorActionPreference='Continue'
  $taskMessages=& $Assembler -a --copy-reflection "$taskRoot\shader-sources\$taskName.reflection.dxbc" $taskAssembly 2>&1
  $taskAssemblerExit=$LASTEXITCODE
 }finally{$ErrorActionPreference=$taskPreference}
 if($taskAssemblerExit -ne 0){throw "Shader assembler failed: $taskName; $taskMessages"}
 $taskShader=Join-Path "$taskOutput\shaders" ($taskName+'.shdr')
 if((Get-FileHash -LiteralPath $taskShader).Hash.ToLowerInvariant() -ne $taskExpected[$taskName]){throw "Shader bytes differ: $taskName"}
}
& "$taskRoot\stock\InstancedPointLights\Build-Native.ps1" -OutputDirectory "$taskOutput\native"
& "$taskRoot\core\Options\Build-HardwareProbe.ps1" -OutputDirectory "$taskOutput\probe"
$taskCsc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$taskManaged=Join-Path $GamePath 'KSP_x64_Data\Managed'
$taskReferences=@('/nologo','/optimize+','/target:library',"/reference:$taskManaged\Assembly-CSharp.dll","/reference:$taskManaged\UnityEngine.CoreModule.dll")
$taskCore="$taskOutput\KerbalOverEngineering.Options.dll"
$taskProbe="$taskOutput\probe\KoeHardwareProbe.dll"
[IO.File]::WriteAllText("$taskOutput\probe.manifest",'KoeHardwareProbe.dll '+(Get-FileHash -LiteralPath $taskProbe).Hash)
$taskSources=@(Get-ChildItem -LiteralPath "$taskRoot\core\Options" -Filter '*.cs' | ForEach-Object {$_.FullName})
& $taskCsc @taskReferences "/out:$taskCore" "/resource:$taskProbe,native.file.KoeHardwareProbe.dll" "/resource:$taskOutput\probe.manifest,native.manifest" @taskSources
if($LASTEXITCODE -ne 0){throw 'Core build failed'}
$taskResources=@();$taskManifest=@()
foreach($taskFile in @((Get-Item "$taskOutput\native\KoeInstancedLights.dll"))+(Get-ChildItem -LiteralPath "$taskOutput\shaders" -Filter '*.shdr')){
 $taskResources+="/resource:$($taskFile.FullName),native.file.$($taskFile.Name)"
 $taskManifest+=($taskFile.Name+' '+(Get-FileHash -LiteralPath $taskFile.FullName).Hash)
}
[IO.File]::WriteAllLines("$taskOutput\native.manifest",$taskManifest)
$taskResources+="/resource:$taskOutput\native.manifest,native.manifest"
$taskPayload="$taskOutput\KoeInstancedPointLights.Runtime.dll"
& $taskCsc @taskReferences "/reference:$taskCore" "/out:$taskPayload" @taskResources "$taskRoot\stock\InstancedPointLights\InstancedPointLights.cs"
if($LASTEXITCODE -ne 0){throw 'Payload build failed'}
[IO.File]::WriteAllText("$taskOutput\module.info","Rendering.InstancedPointLights`nKerbalOverEngineering.InstancedPointLights.Bootstrap")
Copy-Item -LiteralPath "$taskRoot\core\Options\ModuleBootstrap.cs.template" -Destination "$taskOutput\Bootstrap.cs"
& $taskCsc @taskReferences "/reference:$taskCore" "/out:$taskOutput\KoeInstancedPointLights.dll" "/resource:$taskPayload,module.bin" "/resource:$taskOutput\module.info,module.info" "$taskOutput\Bootstrap.cs"
if($LASTEXITCODE -ne 0){throw 'Wrapper build failed'}
Write-Output 'CORRESPONDING_SOURCE_BUILD_PASS: exact four shader bytes + native renderer + core + managed payload/wrapper'

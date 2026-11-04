param([string]$Package,[string]$Output)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot
if(-not $Package){$Package=Join-Path $taskRoot 'dist\GameData\KerbalOverEngineering'}
if(-not $Output){$Output=Join-Path $taskRoot 'dist\KerbalOverEngineering.exe'}
$taskManifest=Join-Path $taskRoot 'artifacts\package38-build\package.manifest'
$taskResources=@();$taskNames=@()
foreach($taskFile in Get-ChildItem -LiteralPath $Package -File -Recurse){
 $taskRelative=$taskFile.FullName.Substring($Package.Length+1).Replace('\','/')
 if($taskRelative -eq 'Options.cfg'){throw 'Do not embed the user configuration'}
 $taskNames+=$taskRelative
 $taskResources+="/resource:$($taskFile.FullName),package.$($taskRelative.Replace('/','.'))"
}
[IO.File]::WriteAllLines($taskManifest,$taskNames,[Text.UTF8Encoding]::new($false))
$taskCsc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
& $taskCsc /nologo /optimize+ /target:exe "/out:$Output" @taskResources "/resource:$taskManifest,package.manifest" "$PSScriptRoot\Installer.cs"
if($LASTEXITCODE -ne 0){throw 'Installer compilation failed'}
Write-Output 'INSTALLER_BUILD_PASS: existing production payloads retained'

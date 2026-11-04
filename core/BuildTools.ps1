function Get-KoeBuildTools {
 $taskVsWhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
 if(-not (Test-Path -LiteralPath $taskVsWhere)){throw 'Install Visual Studio C++ build tools (vswhere.exe is missing)'}
 $taskVS=& $taskVsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
 if(-not $taskVS){throw 'No Visual Studio installation with x64 C++ tools found'}
 $taskCompiler=Get-ChildItem -LiteralPath (Join-Path $taskVS 'VC\Tools\MSVC') -Directory | Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe')} | Sort-Object {[Version]$_.Name} -Descending | Select-Object -First 1
 $taskSDK=(Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
 if(-not $taskSDK){$taskSDK=Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'}
 $taskVersion=Get-ChildItem -LiteralPath (Join-Path $taskSDK 'Include') -Directory | Where-Object {($_.Name -match '^\d+\.\d+\.\d+\.\d+$') -and (Test-Path -LiteralPath (Join-Path $taskSDK ('Lib\'+$_.Name+'\um\x64\kernel32.lib'))) -and (Test-Path -LiteralPath (Join-Path $taskSDK ('Lib\'+$_.Name+'\ucrt\x64\ucrt.lib')))} | Sort-Object {[Version]$_.Name} -Descending | Select-Object -First 1
 if(-not $taskCompiler -or -not $taskVersion){throw 'An x64 MSVC compiler and complete Windows 10/11 SDK are required'}
 [PSCustomObject]@{Compiler=$taskCompiler.FullName;SDK=$taskSDK;Version=$taskVersion.Name}
}

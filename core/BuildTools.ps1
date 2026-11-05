function Get-KoeBuildTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'Install Visual Studio C++ build tools (vswhere.exe is missing)'
    }
    $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $visualStudio) {
        throw 'No Visual Studio installation with x64 C++ tools found'
    }
    $compilerDirectory = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Tools\MSVC') -Directory | Where-Object {
        Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe')
    } | Sort-Object {
        [Version]$_.Name
    } -Descending | Select-Object -First 1
    $windowsSdk = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots' -ErrorAction SilentlyContinue).KitsRoot10
    if (-not $windowsSdk) {
        $windowsSdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    }
    $sdkVersion = Get-ChildItem -LiteralPath (Join-Path $windowsSdk 'Include') -Directory | Where-Object {
        ($_.Name -match '^\d+\.\d+\.\d+\.\d+$') -and (Test-Path -LiteralPath (Join-Path $windowsSdk ('Lib\'+$_.Name+'\um\x64\kernel32.lib'))) -and (Test-Path -LiteralPath (Join-Path $windowsSdk ('Lib\'+$_.Name+'\ucrt\x64\ucrt.lib')))
    } | Sort-Object {
        [Version]$_.Name
    } -Descending | Select-Object -First 1
    if (-not $compilerDirectory -or -not $sdkVersion) {
        throw 'An x64 MSVC compiler and complete Windows 10/11 SDK are required'
    }
    [PSCustomObject] @{
        Compiler = $compilerDirectory.FullName
        SDK = $windowsSdk
        Version = $sdkVersion.Name
    }
}

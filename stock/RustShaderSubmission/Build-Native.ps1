param([string]$OutputDirectory,[string]$Rustc='rustc',[switch]$Audit)
$ErrorActionPreference='Stop'
$repo=Split-Path (Split-Path $PSScriptRoot)
. "$repo\core\BuildTools.ps1"
$tools=Get-KoeBuildTools
if(!$OutputDirectory){$OutputDirectory=Join-Path $repo 'artifacts\rust-shader-submission'}
& "$PSScriptRoot\Build-Rust.ps1" -Rustc $Rustc -OutputDirectory $OutputDirectory
$include=@("$($tools.Compiler)\include","$($tools.SDK)\Include\$($tools.Version)\ucrt","$($tools.SDK)\Include\$($tools.Version)\shared","$($tools.SDK)\Include\$($tools.Version)\um","$repo\third_party\minhook\include")|ForEach-Object{"/I$_"}
$libs=@("$($tools.Compiler)\lib\x64","$($tools.SDK)\Lib\$($tools.Version)\ucrt\x64","$($tools.SDK)\Lib\$($tools.Version)\um\x64")|ForEach-Object{"/LIBPATH:$_"}
$sources=@('buffer.c','hook.c','trampoline.c','hde\hde64.c')|ForEach-Object{"$repo\third_party\minhook\src\$_"}
$defines=@();if($Audit){$defines+='/DKOE_SUBMISSION_AUDIT'}
Push-Location $OutputDirectory
try {
    & "$($tools.Compiler)\bin\Hostx64\x64\cl.exe" /nologo /W3 /O2 /MT /LD /EHsc /std:c++17 @defines @include "$PSScriptRoot\NativeSubmission.cpp" @sources RustSubmission.lib /link @libs bcrypt.lib advapi32.lib userenv.lib ws2_32.lib ntdll.lib /OUT:KoeShaderSubmission.dll
    if($LASTEXITCODE -ne 0){throw 'Native Rust shader submission compilation failed'}
}finally{Pop-Location}
if(!$Audit) {
    $assets=Join-Path $PSScriptRoot 'native-assets'
    New-Item -ItemType Directory -Force $assets | Out-Null
    Copy-Item -LiteralPath "$OutputDirectory\KoeShaderSubmission.dll" -Destination "$assets\KoeShaderSubmission.dll"
    $sourceHashes=@{}
    foreach($name in @('Submission.rs','ValueApply.rs','ValuePlan.rs','UnityAbi.rs','UnityAbi.h','NativeSubmission.cpp','Parallel.rs','PropertyIndex.rs','Build-Rust.ps1','Build-Native.ps1')) {
        $sourceHashes[$name]=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name)).Hash.ToLowerInvariant()
    }
    $manifest=@{binarySha256=(Get-FileHash -LiteralPath "$assets\KoeShaderSubmission.dll").Hash.ToLowerInvariant();sources=$sourceHashes}
    [IO.File]::WriteAllText("$assets\KoeShaderSubmission.build.json",($manifest|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
}

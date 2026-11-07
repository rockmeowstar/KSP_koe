param([Parameter(Mandatory=$true)][string]$GamePath,[string]$Rustc='rustc',[string]$OutputDirectory,[string]$NativeDll)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot
if(!$OutputDirectory){$OutputDirectory=Join-Path $repo 'artifacts\submission-tests'}
$unity=Join-Path $GamePath 'UnityPlayer.dll'
if((Get-FileHash -LiteralPath $unity).Hash.ToLowerInvariant() -ne 'e661981f033e2f1d32db6e44d43c0e0c1783b4cb3115caf393e7722673473317') {
    throw 'The differential fixture requires the verified KSP 1.12.5 Windows Unity image'
}
& "$repo\stock\RustShaderSubmission\Build-Rust.ps1" -Rustc $Rustc -OutputDirectory $OutputDirectory
. "$repo\core\BuildTools.ps1"
$tools=Get-KoeBuildTools
$includes=@("$($tools.Compiler)\include","$($tools.SDK)\Include\$($tools.Version)\ucrt","$($tools.SDK)\Include\$($tools.Version)\shared","$($tools.SDK)\Include\$($tools.Version)\um","$repo\third_party\minhook\include","$repo\stock\RustShaderSubmission") | ForEach-Object{"/I$_"}
$libs=@("$($tools.Compiler)\lib\x64","$($tools.SDK)\Lib\$($tools.Version)\ucrt\x64","$($tools.SDK)\Lib\$($tools.Version)\um\x64") | ForEach-Object{"/LIBPATH:$_"}
$hooks=@('buffer.c','hook.c','trampoline.c','hde\hde64.c') | ForEach-Object {"$repo\third_party\minhook\src\$_"}
Push-Location $OutputDirectory
try {
    & "$($tools.Compiler)\bin\Hostx64\x64\cl.exe" /nologo /W3 /O2 /MT /EHsc /std:c++17 @includes "$PSScriptRoot\ShaderSubmissionTests.cpp" @hooks RustSubmission.lib /link @libs kernel32.lib advapi32.lib userenv.lib ws2_32.lib bcrypt.lib ntdll.lib /OUT:ShaderSubmissionTests.exe
    if($LASTEXITCODE -ne 0){throw 'Submission fixtures compilation failed'}
    & .\ShaderSubmissionTests.exe $unity
    if($LASTEXITCODE -ne 0){throw 'Unity differential fixtures failed'}
    $savedLib=$env:LIB
    try {
        $env:LIB="$($tools.Compiler)\lib\x64;$($tools.SDK)\Lib\$($tools.Version)\ucrt\x64;$($tools.SDK)\Lib\$($tools.Version)\um\x64"
        & $Rustc --edition=2021 --test "$repo\stock\RustShaderSubmission\Submission.rs" -C opt-level=3 -C target-feature=+crt-static "-Clinker=$($tools.Compiler)\bin\Hostx64\x64\link.exe" -o RustSubmissionTests.exe
        if($LASTEXITCODE -ne 0){throw 'Parallel Rust tests compilation failed'}
        & .\RustSubmissionTests.exe
        if($LASTEXITCODE -ne 0){throw 'Parallel Rust tests failed'}
    }finally{$env:LIB=$savedLib}
    if($NativeDll) {
        & "$($tools.Compiler)\bin\Hostx64\x64\cl.exe" /nologo /W3 /O2 /MT /EHsc /std:c++17 @includes "$PSScriptRoot\ShaderSubmissionProductionTests.cpp" /link @libs kernel32.lib /OUT:ShaderSubmissionProductionTests.exe
        if($LASTEXITCODE -ne 0){throw 'Production fixture compilation failed'}
        & .\ShaderSubmissionProductionTests.exe $unity $NativeDll
        if($LASTEXITCODE -ne 0){throw 'Production shader submission fixture failed'}
    }
} finally {Pop-Location}

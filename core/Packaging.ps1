Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Write-KoeZip([string]$SourceDirectory,[string]$Destination,[string[]]$Paths = @('GameData'),[string[]]$ExcludePaths = @()) {
    $sourceRoot = [IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    $destinationPath = [IO.Path]::GetFullPath($Destination)
    $temporaryPath = $destinationPath+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try {
        $archive = [IO.Compression.ZipFile]::Open($temporaryPath,[IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($relativePath in $Paths) {
                $inputPath = [IO.Path]::GetFullPath((Join-Path $sourceRoot $relativePath))
                if (-not $inputPath.StartsWith($sourceRoot,[StringComparison]::OrdinalIgnoreCase)) {
                    throw 'ZIP input must stay inside its source directory'
                }
                $inputFiles = if ((Get-Item -LiteralPath $inputPath).PSIsContainer) {
                    Get-ChildItem -LiteralPath $inputPath -File -Recurse
                }
                else {
                    Get-Item -LiteralPath $inputPath
                }
                foreach ($sourceFile in $inputFiles | Sort-Object FullName) {
                    $entryName = $sourceFile.FullName.Substring($sourceRoot.Length).Replace('\','/')
                    if ($ExcludePaths -contains $entryName) { continue }
                    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$sourceFile.FullName,$entryName,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
                }
            }
        }
        finally {
            $archive.Dispose()
        }
        if (Test-Path -LiteralPath $destinationPath) {
            [IO.File]::Replace($temporaryPath,$destinationPath,[NullString]::Value)
        }
        else {
            [IO.File]::Move($temporaryPath,$destinationPath)
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath) {
            Remove-Item -LiteralPath $temporaryPath
        }
    }
}

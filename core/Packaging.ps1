Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Write-KoeZip([string]$SourceDirectory,[string]$Destination,[string[]]$Paths=@('GameData')) {
 $taskSource=[IO.Path]::GetFullPath($SourceDirectory).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
 $taskDestination=[IO.Path]::GetFullPath($Destination)
 $taskTemporary=$taskDestination+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
 try {
  $taskZip=[IO.Compression.ZipFile]::Open($taskTemporary,[IO.Compression.ZipArchiveMode]::Create)
  try {
   foreach($taskRelative in $Paths){
    $taskPath=[IO.Path]::GetFullPath((Join-Path $taskSource $taskRelative))
    if(-not $taskPath.StartsWith($taskSource,[StringComparison]::OrdinalIgnoreCase)){throw 'ZIP input must stay inside its source directory'}
    $taskFiles=if((Get-Item -LiteralPath $taskPath).PSIsContainer){Get-ChildItem -LiteralPath $taskPath -File -Recurse}else{Get-Item -LiteralPath $taskPath}
    foreach($taskFile in $taskFiles | Sort-Object FullName){
     $taskName=$taskFile.FullName.Substring($taskSource.Length).Replace('\','/')
     [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($taskZip,$taskFile.FullName,$taskName,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
   }
  }finally{$taskZip.Dispose()}
  if(Test-Path -LiteralPath $taskDestination){[IO.File]::Replace($taskTemporary,$taskDestination,[NullString]::Value)}else{[IO.File]::Move($taskTemporary,$taskDestination)}
 }finally{if(Test-Path -LiteralPath $taskTemporary){Remove-Item -LiteralPath $taskTemporary}}
}

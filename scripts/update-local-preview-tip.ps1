# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
 [ValidateSet('Plan','Apply')][string]$Action='Plan',
 [Parameter(Mandatory)][string]$PackagePath,
 [switch]$AcceptUnsignedLocalPreview
)
$ErrorActionPreference='Stop'
$tiRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiProgramFiles=if($env:ProgramW6432){$env:ProgramW6432}else{$env:ProgramFiles}
$tiTarget=[IO.Path]::GetFullPath((Join-Path $tiProgramFiles 'TransInput\0.1.0-local-preview'))
$tiMarker=Join-Path $tiTarget 'local-preview-installation.json'
if($Action -eq 'Apply' -and -not $AcceptUnsignedLocalPreview){throw 'Explicit unsigned-preview acknowledgement required; nothing changed.'}
function Assert-RegularPath([string]$Path){
 $tiWalk=[IO.Path]::GetFullPath($Path)
 while($tiWalk){if(Test-Path -LiteralPath $tiWalk){if((Get-Item -LiteralPath $tiWalk -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse point rejected'}};$tiParent=Split-Path -Parent $tiWalk;if($tiParent -eq $tiWalk){break};$tiWalk=$tiParent}
}
function Hash([string]$Path){Assert-RegularPath $Path;(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Owned-Path([string]$Relative){
 if($Relative -notmatch '^[A-Za-z0-9_./-]+$' -or $Relative -match '(^|/)\.\.?(/|$)' -or [IO.Path]::IsPathRooted($Relative)){throw 'Invalid owned path'}
 $tiPath=[IO.Path]::GetFullPath((Join-Path $tiTarget $Relative))
 if(-not $tiPath.StartsWith($tiTarget+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Owned path escapes installation'}
 Assert-RegularPath $tiPath;return $tiPath
}
Assert-RegularPath $tiMarker
foreach($tiPending in @('tip-update-journal.json','local-preview-installation.pending.json')){if(Test-Path -LiteralPath (Owned-Path $tiPending)){throw 'An unfinished update requires inspection'}}
$tiOriginalMarker=[IO.File]::ReadAllBytes($tiMarker)
$tiState=[Text.Encoding]::UTF8.GetString($tiOriginalMarker).TrimStart([char]0xfeff)|ConvertFrom-Json
if($tiState.schema -ne 1 -or $tiState.channel -cne 'unsigned-local-preview' -or $tiState.target -ine $tiTarget -or $tiState.status -cne 'installed'){throw 'Expected an intact installed local preview'}
$tiSeen=@{}
foreach($tiFile in $tiState.files){
 if($tiSeen.ContainsKey($tiFile.path) -or $tiFile.sha256 -cnotmatch '^[a-f0-9]{64}$'){throw 'Duplicate/invalid installation record'}
 $tiSeen[$tiFile.path]=$true
 if((Hash (Owned-Path $tiFile.path)) -cne $tiFile.sha256){throw ('Installed file differs: '+$tiFile.path)}
}
$PackagePath=[IO.Path]::GetFullPath($PackagePath)
Assert-RegularPath $PackagePath
$tiManifest=Get-Content -LiteralPath (Join-Path $PackagePath 'manifest.json') -Raw|ConvertFrom-Json
if($tiManifest.schema -ne 1 -or $tiManifest.channel -cne 'preview' -or $tiManifest.installed){throw 'Expected a verified preview package'}
$tiCheckpoint=Get-Content -LiteralPath (Join-Path $tiRoot 'artifacts\p1\latest-run.json') -Raw|ConvertFrom-Json
if($tiCheckpoint.status -cne 'passed' -or $tiCheckpoint.scope -cne 'all' -or (@($tiCheckpoint.architectures|Sort-Object -Unique) -join ',') -cne 'x64,x86' -or $tiCheckpoint.path -cne $tiManifest.verification){throw 'Matching complete dual-architecture verification required'}
$tiSources=Get-Content -LiteralPath (Join-Path $tiCheckpoint.path 'source-snapshot.json') -Raw|ConvertFrom-Json
foreach($tiSource in $tiSources.sources){if((Hash (Join-Path $tiRoot $tiSource.path)) -cne $tiSource.sha256){throw ('Source changed since verification: '+$tiSource.path)}}
$tiBinaries=Get-Content -LiteralPath (Join-Path $tiCheckpoint.path 'binary-snapshot.json') -Raw|ConvertFrom-Json
$tiChanges=@()
foreach($tiArchitecture in @('x64','x86')){
 $tiRelative=$tiArchitecture+'/TransInputPrototype.dll'
 $tiEntry=@($tiManifest.files|Where-Object {$_.path -ceq $tiRelative})
 $tiInstalled=@($tiState.files|Where-Object {$_.path -ceq $tiRelative})
 $tiVerified=@($tiBinaries.files|Where-Object {$_.path -ceq ('build/'+$tiRelative)})
 if($tiEntry.Count -ne 1 -or $tiInstalled.Count -ne 1 -or $tiVerified.Count -ne 1 -or $tiEntry[0].sha256 -cne $tiVerified[0].sha256){throw 'Missing/ambiguous verified TIP binary'}
 $tiInput=Join-Path $PackagePath $tiRelative
 if((Hash $tiInput) -cne $tiEntry[0].sha256){throw 'New TIP checksum mismatch'}
 if($tiInstalled[0].sha256 -ceq $tiEntry[0].sha256){continue}
 $tiBackup=$tiArchitecture+'/TransInputPrototype.before-'+$tiInstalled[0].sha256+'.dll'
 $tiStage=$tiArchitecture+'/TransInputPrototype.pending-'+$tiEntry[0].sha256+'.dll'
 foreach($tiName in @($tiBackup,$tiStage)){if(Test-Path -LiteralPath (Owned-Path $tiName)){throw 'Update staging/backup already exists; inspect before retrying'}}
 $tiChanges+=[pscustomobject]@{architecture=$tiArchitecture;path=$tiRelative;input=$tiInput;old_hash=$tiInstalled[0].sha256;new_hash=$tiEntry[0].sha256;backup=$tiBackup;stage=$tiStage;renamed=$false;swapped=$false}
}
$tiPlan=[ordered]@{action=$Action;target=$tiTarget;changes=@($tiChanges|Select-Object architecture,path,old_hash,new_hash,backup);restarts_apps=$false;registry_changes=$false;network_requests=$false;executed=$false}
if($Action -eq 'Plan' -or -not $tiChanges.Count){$tiPlan|ConvertTo-Json -Depth 6;return}
$tiEvidence=Join-Path $tiRoot ('artifacts\tip-update\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $tiEvidence|Out-Null
[IO.File]::WriteAllBytes((Join-Path $tiEvidence 'installation-before.json'),$tiOriginalMarker)
$tiPlan|ConvertTo-Json -Depth 6|Set-Content (Join-Path $tiEvidence 'plan.json') -Encoding UTF8
$tiJournal=Owned-Path 'tip-update-journal.json'
if(Test-Path -LiteralPath $tiJournal){throw 'An unfinished TIP update requires inspection'}
function Save-Journal { $tiChanges|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $tiJournal -Encoding UTF8 }
try {
 # Stage both verified DLLs before changing either installed path. A loaded old
 # image is renamed, never truncated, deleted, or modified in a user process.
 foreach($tiChange in $tiChanges){$tiOutput=Owned-Path $tiChange.stage;Copy-Item -LiteralPath $tiChange.input -Destination $tiOutput;if((Hash $tiOutput) -cne $tiChange.new_hash){throw 'Staged TIP checksum mismatch'}}
 Save-Journal
 foreach($tiChange in $tiChanges){
  Move-Item -LiteralPath (Owned-Path $tiChange.path) -Destination (Owned-Path $tiChange.backup);$tiChange.renamed=$true;Save-Journal
  Move-Item -LiteralPath (Owned-Path $tiChange.stage) -Destination (Owned-Path $tiChange.path);$tiChange.swapped=$true;Save-Journal
 }
 foreach($tiChange in $tiChanges){
  $tiHelper=Owned-Path ($tiChange.architecture+'/TransInputRegister.exe')
  & $tiHelper --verify-registered *> (Join-Path $tiEvidence ($tiChange.architecture+'-registered.txt'))
  if($LASTEXITCODE -ne 0){throw 'Updated registered COM activation failed'}
  $tiEntry=$tiState.files|Where-Object {$_.path -ceq $tiChange.path};$tiEntry.sha256=$tiChange.new_hash
  $tiState.files+= [pscustomobject]@{path=$tiChange.backup;sha256=$tiChange.old_hash}
 }
 $tiUpdate=[ordered]@{installed_at=(Get-Date).ToString('o');verification=$tiCheckpoint.path;evidence=$tiEvidence;requires_app_restart=$true;components=@($tiChanges|Select-Object path,new_hash,backup)}
 $tiState|Add-Member -NotePropertyName tip_update -NotePropertyValue $tiUpdate -Force
 $tiTemporary=Owned-Path 'local-preview-installation.pending.json'
 $tiState|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $tiTemporary -Encoding UTF8
 Move-Item -LiteralPath $tiTemporary -Destination $tiMarker -Force
 foreach($tiFile in $tiState.files){if((Hash (Owned-Path $tiFile.path)) -cne $tiFile.sha256){throw 'Post-update file verification failed'}}
 Remove-Item -LiteralPath $tiJournal
 [ordered]@{success=$true;updated_tip_architectures=@($tiChanges.architecture);files_verified=@($tiState.files).Count;requires_app_restart=$true;other_installed_components_unchanged=$true;evidence=$tiEvidence}|ConvertTo-Json -Depth 5|Tee-Object (Join-Path $tiEvidence 'result.json')
} catch {
 $tiFailure=$_.Exception.Message;$tiRollback=@()
 for($tiIndex=$tiChanges.Count-1;$tiIndex -ge 0;$tiIndex--){$tiChange=$tiChanges[$tiIndex];try {
  if($tiChange.swapped){$tiPath=Owned-Path $tiChange.path;if((Hash $tiPath) -cne $tiChange.new_hash){throw 'Unexpected replacement file'};Remove-Item -LiteralPath $tiPath}
  if($tiChange.renamed){$tiBackupPath=Owned-Path $tiChange.backup;if((Hash $tiBackupPath) -cne $tiChange.old_hash){throw 'Unexpected backup file'};Move-Item -LiteralPath $tiBackupPath -Destination (Owned-Path $tiChange.path)}
  $tiStagePath=Owned-Path $tiChange.stage;if(Test-Path -LiteralPath $tiStagePath){if((Hash $tiStagePath) -cne $tiChange.new_hash){throw 'Unexpected staged file'};Remove-Item -LiteralPath $tiStagePath}
 }catch {$tiRollback+=$tiChange.architecture}}
 if(-not $tiRollback.Count){[IO.File]::WriteAllBytes($tiMarker,$tiOriginalMarker);if(Test-Path -LiteralPath $tiJournal){Remove-Item -LiteralPath $tiJournal}}
 [ordered]@{success=$false;error=$tiFailure;rollback_errors=$tiRollback}|ConvertTo-Json|Set-Content (Join-Path $tiEvidence 'failure.json') -Encoding UTF8
 throw ('TIP update failed: '+$tiFailure+'; evidence: '+$tiEvidence)
}

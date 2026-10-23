# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
 [ValidateSet('Plan','Install','Verify','Disable','Enable','Uninstall')][string]$Action='Plan',
 [string]$PackagePath,
 [switch]$AcceptUnsignedLocalPreview
)
$ErrorActionPreference='Stop'
$tiSourceRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiProgramFiles=$env:ProgramW6432
if(-not $tiProgramFiles){$tiProgramFiles=$env:ProgramFiles}
$tiBase=[IO.Path]::GetFullPath((Join-Path $tiProgramFiles 'TransInput'))
$tiTarget=[IO.Path]::GetFullPath((Join-Path $tiBase '0.1.0-local-preview'))
$tiMarker=Join-Path $tiTarget 'local-preview-installation.json'
$tiRunKey='HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$tiRunName='TransInputSettings'
$tiUninstallKey='HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\TransInputLocalPreview'
$tiPlan=[ordered]@{schema=1;action=$Action;target=$tiTarget;channel='unsigned-local-preview';existing_permissions_only=$true;sets_default=$false;creates_start_menu=$false;changes_trust_or_policy=$false;preserves_user_data=$true;executed=$false}
if($Action -eq 'Plan'){$tiPlan|ConvertTo-Json;return}
if($Action -ne 'Verify' -and -not $AcceptUnsignedLocalPreview){throw 'Explicit -AcceptUnsignedLocalPreview is required; no change performed.'}
function Assert-NoReparse([string]$Path){
 $tiWalk=[IO.Path]::GetFullPath($Path)
 while($tiWalk){if(Test-Path -LiteralPath $tiWalk){if((Get-Item -LiteralPath $tiWalk -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Deployment path contains a reparse point.'}};$tiParent=Split-Path -Parent $tiWalk;if($tiParent -eq $tiWalk){break};$tiWalk=$tiParent}
}
Assert-NoReparse $tiTarget
$tiEvidenceRoot=if(Test-Path -LiteralPath (Join-Path $tiSourceRoot 'Cargo.toml')){Join-Path $tiSourceRoot 'artifacts\installation'}else{Join-Path $env:LOCALAPPDATA 'TransInputNext\deployment'}
$tiEvidence=Join-Path $tiEvidenceRoot (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Path $tiEvidence -Force|Out-Null
$tiSteps=[Collections.Generic.List[object]]::new()
function Invoke-Helper([string]$Architecture,[string]$Operation,[switch]$AllowFailure,[int]$ReadRetry=0){
 $tiExecutable=Join-Path $tiTarget ($Architecture+'\TransInputRegister.exe')
 $tiArguments=@($Operation);if($Operation -ne '--verify-registered'){$tiArguments+='--acknowledge-unsigned-local-preview'}
 $tiName=('{0:D2}-{1}-{2}' -f $tiSteps.Count,$Architecture,$Operation.TrimStart('-'))
 $tiProcess=Start-Process -FilePath $tiExecutable -ArgumentList $tiArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $tiEvidence ($tiName+'.out.txt')) -RedirectStandardError (Join-Path $tiEvidence ($tiName+'.err.txt'))
 $tiHandle=$tiProcess.Handle
 if(-not $tiProcess.WaitForExit(20000)){Stop-Process -Id $tiProcess.Id -Force;throw ('Owned registration helper timed out: '+$tiName)}
 $tiProcess.WaitForExit();$tiCode=$tiProcess.ExitCode
 if($null -eq $tiCode){throw ('Missing helper exit code: '+$tiHandle)}
 $tiSteps.Add([ordered]@{architecture=$Architecture;operation=$Operation;exit_code=$tiCode})
 $tiSteps|ConvertTo-Json -Depth 5|Set-Content (Join-Path $tiEvidence 'steps.json') -Encoding UTF8
 # Windows can briefly return E_FAIL for GetProfile immediately after the
 # cross-architecture registration broadcast. Retry only this read-only check
 # in a fresh process; never retry registration, edits or paid requests.
 if($Operation -eq '--verify-registered' -and $tiCode -eq 1 -and $ReadRetry -lt 2){Start-Sleep -Milliseconds 250;return (Invoke-Helper $Architecture $Operation -AllowFailure:$AllowFailure -ReadRetry ($ReadRetry+1))}
 if($tiCode -ne 0 -and -not $AllowFailure){throw ('Registration operation failed: '+$tiName+'; see '+$tiEvidence)}
 return $tiCode
}
function Get-UserStatus {
 $tiStatusText=& (Join-Path $PSScriptRoot 'ime-user-status.ps1')
 return ($tiStatusText -join "`n"|ConvertFrom-Json)
}
function Read-OwnedState([switch]$AllowMissing){
 if(-not (Test-Path -LiteralPath $tiMarker -PathType Leaf)){throw 'No owned local-preview installation record.'}
 $tiState=Get-Content -LiteralPath $tiMarker -Raw|ConvertFrom-Json
 if($tiState.schema -ne 1 -or $tiState.channel -cne 'unsigned-local-preview' -or $tiState.target -ine $tiTarget){throw 'Installation ownership mismatch'}
 foreach($tiFile in $tiState.files){
  $tiPath=[IO.Path]::GetFullPath((Join-Path $tiTarget $tiFile.path))
  if(-not $tiPath.StartsWith($tiTarget+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Installation record path escapes its root'}
  Assert-NoReparse $tiPath
  if(-not (Test-Path -LiteralPath $tiPath -PathType Leaf)){if($AllowMissing){continue};throw ('Installed file missing: '+$tiFile.path)}
  if((Get-FileHash -LiteralPath $tiPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $tiFile.sha256){throw ('Installed file changed: '+$tiFile.path)}
 }
 return $tiState
}
function Verify-Installation {
 foreach($tiArchitecture in @('x64','x86')){$null=Invoke-Helper $tiArchitecture '--verify-registered'}
 $tiStatus=Get-UserStatus
 if(-not $tiStatus.transinput -or -not $tiStatus.microsoft_pinyin){throw 'Expected current-user TransInput and Microsoft Pinyin were not both enabled'}
 return $tiStatus
}
function Remove-OwnedFiles($State){
 # Check every absolute deletion target; never traverse unknown files/junctions.
 $tiDeletionOrder=@($State.files|Sort-Object @{Expression={if($_.path -match '^x(64|86)/.*\.dll$'){0}elseif($_.path -match '\.(dll|exe)$'){1}elseif($_.path -like 'maintenance/*'){3}else{2}}},path)
 foreach($tiFile in $tiDeletionOrder){$tiPath=[IO.Path]::GetFullPath((Join-Path $tiTarget $tiFile.path));if(-not $tiPath.StartsWith($tiTarget+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Unsafe cleanup target'};Assert-NoReparse $tiPath;if(Test-Path -LiteralPath $tiPath -PathType Leaf){Remove-Item -LiteralPath $tiPath -Force}}
 Remove-Item -LiteralPath $tiMarker -Force
 foreach($tiDirectory in @(Get-ChildItem -LiteralPath $tiTarget -Directory -Recurse -Force|Sort-Object {$_.FullName.Length} -Descending)){
  if(-not $tiDirectory.FullName.StartsWith($tiTarget+'\',[StringComparison]::OrdinalIgnoreCase) -or ($tiDirectory.Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Unsafe cleanup directory'}
  if(@(Get-ChildItem -LiteralPath $tiDirectory.FullName -Force).Count -eq 0){Remove-Item -LiteralPath $tiDirectory.FullName}
 }
 if(@(Get-ChildItem -LiteralPath $tiTarget -Force).Count -eq 0){Remove-Item -LiteralPath $tiTarget}
}
if($Action -ne 'Install'){
 $tiState=Read-OwnedState -AllowMissing:($Action -eq 'Uninstall')
 switch($Action){
  'Verify' {$null=Verify-Installation}
  'Disable' {$null=Invoke-Helper 'x64' '--disable-local-preview'}
  'Enable' {$null=Invoke-Helper 'x64' '--enable-local-preview';$null=Verify-Installation}
  'Uninstall' {
   if($tiState.status -ne 'unregistered'){
    $null=Invoke-Helper 'x64' '--disable-local-preview'
    foreach($tiArchitecture in @('x86','x64')){$null=Invoke-Helper $tiArchitecture '--unregister-local-preview'}
    $tiState.status='unregistered';$tiState|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $tiMarker -Encoding UTF8
   }
   $tiRun=(Get-ItemProperty -LiteralPath $tiRunKey -Name $tiRunName -ErrorAction SilentlyContinue).$tiRunName
   if($tiRun -eq $tiState.run_command){Remove-ItemProperty -LiteralPath $tiRunKey -Name $tiRunName}
   if(Test-Path -LiteralPath $tiUninstallKey){$tiEntry=Get-ItemProperty -LiteralPath $tiUninstallKey;if($tiEntry.InstallLocation -ine $tiTarget){throw 'Uninstall entry ownership mismatch'}}
   foreach($tiProcess in @(Get-CimInstance Win32_Process|Where-Object {$_.Name -in @('TransInputSettings.exe','TransInputBroker.exe','TransInputTranslator.exe') -and $_.ExecutablePath -and $_.ExecutablePath.StartsWith($tiTarget+'\',[StringComparison]::OrdinalIgnoreCase)})){Stop-Process -Id $tiProcess.ProcessId -Force}
   # Verify before removing maintenance scripts; the installed uninstaller
   # executes from this same tree and cannot query a deleted status script.
   $tiStatus=Get-UserStatus;if($tiStatus.transinput -or -not $tiStatus.microsoft_pinyin){throw 'User input method uninstall verification failed'}
   Remove-OwnedFiles $tiState
   if(Test-Path -LiteralPath $tiUninstallKey){Remove-Item -LiteralPath $tiUninstallKey}
  }
 }
 [ordered]@{action=$Action;success=$true;evidence=$tiEvidence;sets_default=$false;changes_trust_or_policy=$false}|ConvertTo-Json;return
}
if(-not $PackagePath){throw 'Install requires the verified preview package directory'}
if($PSVersionTable.PSVersion.Major -lt 7){throw 'Local preview installation uses the existing PowerShell 7 runtime; no execution-policy change is performed.'}
$PackagePath=[IO.Path]::GetFullPath($PackagePath);Assert-NoReparse $PackagePath
if(Test-Path -LiteralPath $tiTarget){throw 'Preview version already exists; verify or explicitly uninstall it before reinstalling.'}
if(Test-Path -LiteralPath $tiUninstallKey){throw 'An existing local-preview uninstall record must be resolved first'}
$tiExistingRun=(Get-ItemProperty -LiteralPath $tiRunKey -Name $tiRunName -ErrorAction SilentlyContinue).$tiRunName
if($tiExistingRun){throw 'Existing settings startup command must not be overwritten'}
foreach($tiView in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
 $tiMachine=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$tiView)
 try {foreach($tiKeyName in @('SOFTWARE\Classes\CLSID\{40E2B4B8-B417-47D3-AACF-F49A77858E40}','SOFTWARE\Microsoft\CTF\TIP\{40E2B4B8-B417-47D3-AACF-F49A77858E40}')){$tiKey=$tiMachine.OpenSubKey($tiKeyName);if($tiKey){$tiKey.Dispose();throw 'Refusing to overwrite an existing TransInput registration'}}}finally{$tiMachine.Dispose()}
}
$tiBefore=Get-UserStatus
if(-not $tiBefore.microsoft_pinyin){throw 'Microsoft Pinyin fallback must remain available'}
$tiManifest=Get-Content -LiteralPath (Join-Path $PackagePath 'manifest.json') -Raw|ConvertFrom-Json
if($tiManifest.schema -ne 1 -or $tiManifest.channel -cne 'preview' -or $tiManifest.installed){throw 'Expected a verified development preview package'}
$tiRequired=@('x64/TransInputPrototype.dll','x86/TransInputPrototype.dll','x64/TransInputRegister.exe','x86/TransInputRegister.exe','shared/TransInputBroker.exe','shared/TransInputTranslator.exe','shared/TransInputSettings.exe','shared/transinput_core.dll','shared/transinput-icon.png')
foreach($tiName in $tiRequired){if(@($tiManifest.files|Where-Object {$_.path -ceq $tiName}).Count -ne 1){throw 'Package is missing a unique required component'}}
$tiFiles=@();$tiSeen=@{}
foreach($tiFile in $tiManifest.files){
 if($tiFile.path -notmatch '^[A-Za-z0-9_./-]+$' -or $tiFile.path -match '(^|/)\.\.(/|$)' -or [IO.Path]::IsPathRooted($tiFile.path) -or $tiFile.sha256 -notmatch '^[a-f0-9]{64}$' -or $tiSeen.ContainsKey($tiFile.path)){throw 'Invalid or duplicate package path/hash'}
 $tiSeen[$tiFile.path]=$true;$tiPath=[IO.Path]::GetFullPath((Join-Path $PackagePath $tiFile.path))
 if(-not $tiPath.StartsWith($PackagePath+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Package path escapes its root'}
 Assert-NoReparse $tiPath
 if((Get-FileHash -LiteralPath $tiPath -Algorithm SHA256).Hash.ToLowerInvariant() -cne $tiFile.sha256){throw ('Package checksum mismatch: '+$tiFile.path)}
 $tiFiles+=[pscustomobject]@{path=$tiFile.path;sha256=$tiFile.sha256}
}
foreach($tiName in @('install-local-preview.ps1','ime-user-status.ps1')){$tiPath=Join-Path $PSScriptRoot $tiName;$tiFiles+=[pscustomobject]@{path=('maintenance/'+$tiName);sha256=(Get-FileHash -LiteralPath $tiPath -Algorithm SHA256).Hash.ToLowerInvariant()}}
$tiRunCommand='"'+(Join-Path $tiTarget 'shared\TransInputSettings.exe')+'" --tray'
$tiState=[ordered]@{schema=1;channel='unsigned-local-preview';target=$tiTarget;created_at=(Get-Date).ToString('o');files=$tiFiles;run_command=$tiRunCommand;status='staging';package_verification=$tiManifest.verification}
$tiAttempted=[Collections.Generic.List[string]]::new();$tiCreated=$false;$tiRunCreated=$false;$tiUninstallCreated=$false
try {
 New-Item -ItemType Directory -Path $tiTarget -Force|Out-Null;$tiCreated=$true
 $tiState|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $tiMarker -Encoding UTF8
 foreach($tiFile in $tiFiles){$tiInput=if($tiFile.path.StartsWith('maintenance/')){Join-Path $PSScriptRoot ([IO.Path]::GetFileName($tiFile.path))}else{Join-Path $PackagePath $tiFile.path};$tiOutput=Join-Path $tiTarget $tiFile.path;New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($tiOutput)) -Force|Out-Null;Copy-Item -LiteralPath $tiInput -Destination $tiOutput}
 $null=Read-OwnedState # Verify the copied bytes before executing any helper.
 foreach($tiArchitecture in @('x64','x86')){$tiAttempted.Add($tiArchitecture);$null=Invoke-Helper $tiArchitecture '--register-local-preview'}
 $null=Invoke-Helper 'x64' '--enable-local-preview'
 $tiAfter=Verify-Installation
 foreach($tiTip in $tiBefore.tips){if($tiAfter.tips -notcontains $tiTip){throw 'An existing enabled input method disappeared'}}
 New-Item -Path $tiRunKey -Force|Out-Null;New-ItemProperty -LiteralPath $tiRunKey -Name $tiRunName -Value $tiRunCommand -PropertyType String|Out-Null;$tiRunCreated=$true
 New-Item -Path $tiUninstallKey -Force|Out-Null;$tiUninstallCreated=$true
 $tiShell=(Get-Process -Id $PID).Path
 if([IO.Path]::GetFileName($tiShell) -ine 'pwsh.exe'){throw 'Expected the existing PowerShell 7 host'}
 $tiUninstall='"'+$tiShell+'" -NoProfile -File "'+(Join-Path $tiTarget 'maintenance\install-local-preview.ps1')+'" -Action Uninstall -AcceptUnsignedLocalPreview'
 $tiDisplayName='TransInput ('+(-join [char[]]@(0x672C,0x673A,0x9A8C,0x6536,0x7248))+')'
 foreach($tiPair in @{DisplayName=$tiDisplayName;DisplayVersion='0.1.0-local-preview';Publisher='jihongit';InstallLocation=$tiTarget;UninstallString=$tiUninstall;DisplayIcon=(Join-Path $tiTarget 'shared\TransInputSettings.exe')}.GetEnumerator()){New-ItemProperty -LiteralPath $tiUninstallKey -Name $tiPair.Key -Value $tiPair.Value -PropertyType String|Out-Null}
 $tiState.status='installed';$tiState|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $tiMarker -Encoding UTF8
 $null=Read-OwnedState
 Start-Process -FilePath (Join-Path $tiTarget 'shared\TransInputSettings.exe') -ArgumentList '--tray' -WindowStyle Hidden|Out-Null
 [ordered]@{action='Install';success=$true;installed=$true;channel='unsigned-local-preview';target=$tiTarget;evidence=$tiEvidence;sets_default=$false;changes_trust_or_policy=$false;new_network_requests=$false}|ConvertTo-Json|Tee-Object -FilePath (Join-Path $tiEvidence 'result.json')
} catch {
 $tiOriginalError=$_.Exception.Message;$tiRollback=@()
 if($tiRunCreated){Remove-ItemProperty -LiteralPath $tiRunKey -Name $tiRunName -ErrorAction SilentlyContinue}
 if($tiUninstallCreated){Remove-Item -LiteralPath $tiUninstallKey -ErrorAction SilentlyContinue}
 if($tiAttempted.Count){try {$null=Invoke-Helper 'x64' '--disable-local-preview' -AllowFailure}catch {$tiRollback+='disable-error'}}
 for($tiIndex=$tiAttempted.Count-1;$tiIndex -ge 0;$tiIndex--){try {$tiCode=Invoke-Helper $tiAttempted[$tiIndex] '--unregister-local-preview' -AllowFailure;if($tiCode){$tiRollback+=('unregister-'+$tiAttempted[$tiIndex])}}catch {$tiRollback+=('unregister-'+$tiAttempted[$tiIndex])}}
 if($tiCreated -and -not $tiRollback.Count){try {# Delete only successfully copied owned files; preserve unknown contents.
   $tiPartial=[pscustomobject]@{files=@($tiFiles|Where-Object {Test-Path -LiteralPath (Join-Path $tiTarget $_.path) -PathType Leaf})};Remove-OwnedFiles $tiPartial
  }catch {$tiRollback+='file-cleanup-error'}}
 [ordered]@{success=$false;error=$tiOriginalError;rollback_errors=$tiRollback;target=$tiTarget;installed=$false}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $tiEvidence 'failure.json') -Encoding UTF8
 throw ('Local-preview installation failed: '+$tiOriginalError+'; evidence: '+$tiEvidence)
}

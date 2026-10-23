# SPDX-License-Identifier: AGPL-3.0-only
# Safety boundaries only; never invokes a registration helper in Program Files.
[CmdletBinding()]
param([ValidateSet('x64','x86')][string]$Architecture='x64')
$ErrorActionPreference='Stop'
$tiRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiExecutable=Join-Path $tiRoot ('build\'+$Architecture+'\TransInputRegister.exe')
$tiTarget=Join-Path $env:ProgramW6432 'TransInput\0.1.0-local-preview'
$tiExisted=Test-Path -LiteralPath $tiTarget
function Read-Registration {
 $tiValues=@()
 foreach($tiView in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
  $tiMachine=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$tiView)
  try {$tiKey=$tiMachine.OpenSubKey('SOFTWARE\Classes\CLSID\{40E2B4B8-B417-47D3-AACF-F49A77858E40}\InprocServer32');try {$tiValues+=if($tiKey){$tiKey.GetValue('')}else{'absent'}}finally{if($tiKey){$tiKey.Dispose()}}}finally{$tiMachine.Dispose()}
 }
 return ($tiValues -join '|')
}
$tiBefore=Read-Registration
$tiEvidence=Join-Path $tiRoot ('artifacts\preview-boundaries\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+$Architecture)
New-Item -ItemType Directory -Path $tiEvidence -Force|Out-Null
$tiPlan=& (Join-Path $PSScriptRoot 'install-local-preview.ps1') -Action Plan|ConvertFrom-Json
if($tiPlan.executed -or $tiPlan.sets_default -or $tiPlan.changes_trust_or_policy -or $tiPlan.creates_start_menu){throw 'Plan has unsafe effects'}
$tiDenied=$false
try {& (Join-Path $PSScriptRoot 'install-local-preview.ps1') -Action Install}catch {if($_.Exception.Message -like 'Explicit -AcceptUnsignedLocalPreview*'){$tiDenied=$true}else{throw}}
if(-not $tiDenied){throw 'Unsigned preview did not require acknowledgement'}
$tiDenied=$false
try {& (Join-Path $PSScriptRoot 'update-local-preview-tip.ps1') -Action Apply -PackagePath 'fixed-missing-preview'}catch {if($_.Exception.Message -like 'Explicit unsigned-preview acknowledgement*'){$tiDenied=$true}else{throw}}
if(-not $tiDenied){throw 'Unsigned TIP update did not require acknowledgement'}
$tiCaseIndex=0
foreach($tiCase in @(
 @{arguments=@('--register-local-preview');code=2},
 @{arguments=@('--register-local-preview','--acknowledge-unsigned-local-preview');code=5},
 @{arguments=@('--register-signed');code=5},
 @{arguments=@('--verify-registered');code=5}
)){
 $tiProcess=Start-Process -FilePath $tiExecutable -ArgumentList $tiCase.arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $tiEvidence ($tiCaseIndex.ToString()+'.out.txt')) -RedirectStandardError (Join-Path $tiEvidence ($tiCaseIndex.ToString()+'.err.txt'))
 $tiHandle=$tiProcess.Handle
 if(-not $tiProcess.WaitForExit(10000)){Stop-Process -Id $tiProcess.Id -Force;throw 'Owned boundary helper timed out'}
 $tiProcess.WaitForExit();if($tiProcess.ExitCode -ne $tiCase.code){throw ('Unexpected boundary exit: '+$tiProcess.ExitCode+' handle '+$tiHandle)}
 $tiCaseIndex++
}
if((Read-Registration) -cne $tiBefore -or (Test-Path -LiteralPath $tiTarget) -ne $tiExisted){throw 'Boundary check changed installation state'}
'PASS: preview requires acknowledgement; source directory/signed release/verification location guards reject; plan does not install; existing registration preserved'

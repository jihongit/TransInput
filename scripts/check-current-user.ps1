# SPDX-License-Identifier: AGPL-3.0-only
# Read-only check. Run from the user's ordinary PowerShell session.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$tiRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiIdentity=[Security.Principal.WindowsIdentity]::GetCurrent()
try {
 $tiPrincipal=[Security.Principal.WindowsPrincipal]::new($tiIdentity)
 $tiAdmin=$tiPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} finally {$tiIdentity.Dispose()}
$tiAccess=@()
foreach($tiView in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
 $tiMachine=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$tiView)
 try {
  foreach($tiPath in @('SOFTWARE\Classes','SOFTWARE\Microsoft\CTF\TIP')){
   $tiKey=$null;$tiAllowed=$false;$tiError=$null
   try {$tiKey=$tiMachine.OpenSubKey($tiPath,[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[Security.AccessControl.RegistryRights]::CreateSubKey);$tiAllowed=$null -ne $tiKey}
   catch {$tiError=$_.Exception.GetType().Name}
   finally {if($tiKey){$tiKey.Dispose()}}
   $tiAccess+=[ordered]@{view=$tiView.ToString();path=$tiPath;create_access_opened=$tiAllowed;error_type=$tiError}
  }
 } finally {$tiMachine.Dispose()}
}
$tiUserKey=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software',$true)
try {$tiUserWritable=$null -ne $tiUserKey} finally {if($tiUserKey){$tiUserKey.Dispose()}}
$tiResult=[ordered]@{schema=1;checked_at=(Get-Date).ToString('o');read_only=$true;administrator_role_enabled=$tiAdmin;current_user_software_writable=$tiUserWritable;machine_access=$tiAccess;installs_or_registers_ime=$false;changes_trust_or_policy=$false;network_requests=$false}
$tiDirectory=Join-Path $tiRoot 'artifacts\user-deployment'
New-Item -ItemType Directory -Path $tiDirectory -Force|Out-Null
$tiResult|ConvertTo-Json -Depth 6|Set-Content (Join-Path $tiDirectory 'ordinary-session.json') -Encoding UTF8
Write-Output ('Administrator role enabled: '+$tiAdmin)
Write-Output ('Current-user Software writable: '+$tiUserWritable)
Write-Output 'Read-only result saved to artifacts\user-deployment\ordinary-session.json. No input method was installed or changed.'

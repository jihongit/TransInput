# SPDX-License-Identifier: AGPL-3.0-only
# Run with Windows PowerShell for the supported International module.
$ErrorActionPreference='Stop'
if($PSVersionTable.PSVersion.Major -ge 6){
 # The Windows International module's native marshalling is incompatible with
 # PowerShell 7. Use only this fixed read-only command in its supported runtime.
 $tiText=& powershell.exe -NoProfile -Command 'Get-WinUserLanguageList | ForEach-Object { $_.InputMethodTips } | ConvertTo-Json'
 if($LASTEXITCODE -ne 0){throw 'Current-user input language query failed'}
 $tiTips=@(($tiText -join "`n")|ConvertFrom-Json)
}else{$tiTips=@(Get-WinUserLanguageList|ForEach-Object {$_.InputMethodTips}|ForEach-Object {$_})}
[ordered]@{tips=$tiTips;microsoft_pinyin=(@($tiTips|Where-Object {$_ -match '81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E'}).Count -gt 0);transinput=(@($tiTips|Where-Object {$_ -match '40E2B4B8-B417-47D3-AACF-F49A77858E40'}).Count -gt 0)}|ConvertTo-Json -Depth 4

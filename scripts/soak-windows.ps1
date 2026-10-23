# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
    [ValidateSet('x64','x86','all')][string]$Architecture='all',
    [ValidateRange(10,28800)][int]$Seconds=60
)
$ErrorActionPreference='Stop'
$tiRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiEvidence=Join-Path $tiRoot ('artifacts\soak\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $tiEvidence -Force | Out-Null
$tiArchitectures=if($Architecture -eq 'all'){@('x64','x86')}else{@($Architecture)}
$tiResults=[Collections.Generic.List[object]]::new()
foreach($tiArch in $tiArchitectures){
    $tiExecutable=Join-Path $tiRoot "build\$tiArch\TransInputTestHost.exe"
    if(-not (Test-Path -LiteralPath $tiExecutable)){throw 'Build and verify the requested architectures first.'}
    $tiOutput=Join-Path $tiEvidence "$tiArch.stdout.txt"
    $tiError=Join-Path $tiEvidence "$tiArch.stderr.txt"
    $tiTimer=[Diagnostics.Stopwatch]::StartNew()
    $tiProcess=Start-Process -FilePath $tiExecutable -ArgumentList "--soak-seconds=$Seconds" -WorkingDirectory $tiRoot `
        -WindowStyle Hidden -RedirectStandardOutput $tiOutput -RedirectStandardError $tiError -PassThru
    $tiHandle=$tiProcess.Handle
    if(-not $tiProcess.WaitForExit(($Seconds+30)*1000)){
        & taskkill.exe /PID $tiProcess.Id /T /F | Out-Null
        throw "Owned soak exceeded its bound; evidence: $tiEvidence"
    }
    $tiProcess.WaitForExit();$tiTimer.Stop()
    if($null -eq $tiProcess.ExitCode){throw "Missing exit code (handle $tiHandle)"}
    $tiResults.Add([ordered]@{architecture=$tiArch;seconds=$Seconds;elapsed_ms=$tiTimer.ElapsedMilliseconds;exit_code=$tiProcess.ExitCode;sha256=(Get-FileHash -LiteralPath $tiExecutable -Algorithm SHA256).Hash.ToLowerInvariant()})
    $tiResults | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $tiEvidence 'checks.json') -Encoding UTF8
    Write-Output "$tiArch soak: exit $($tiProcess.ExitCode), $($tiTimer.ElapsedMilliseconds)ms; $tiOutput"
    if($tiProcess.ExitCode -ne 0){Get-Content $tiError;throw 'Owned native-editor soak failed.'}
}
Write-Output "Evidence: $tiEvidence"

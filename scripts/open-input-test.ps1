# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
    [ValidateSet('x64','x86')][string]$Architecture='x64',
    [switch]$NoOpen
)
$ErrorActionPreference='Stop'
$tiRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiSource=Join-Path $tiRoot "build\$Architecture"
$tiFiles=@('TransInputTestHost.exe','TransInputPrototypeTest.dll','TransInputBroker.exe','transinput_core.dll','TransInputTranslationFixture.exe')
foreach($tiFile in $tiFiles){if(-not (Test-Path -LiteralPath (Join-Path $tiSource $tiFile))){throw 'Run scripts/verify-windows.ps1 for this architecture first.'}}
$tiDirectory=Join-Path $tiRoot ('artifacts\manual-input\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+$Architecture)
New-Item -ItemType Directory -Path $tiDirectory | Out-Null
$tiManifest=foreach($tiFile in $tiFiles){
    $tiTarget=Join-Path $tiDirectory $tiFile
    Copy-Item -LiteralPath (Join-Path $tiSource $tiFile) -Destination $tiTarget
    [ordered]@{name=$tiFile;sha256=(Get-FileHash -LiteralPath $tiTarget -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$tiManifest | ConvertTo-Json | Set-Content (Join-Path $tiDirectory 'files.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $tiRoot 'docs\testing\INPUT_PAD.md') -Destination (Join-Path $tiDirectory 'README.md')
$tiExe=Join-Path $tiDirectory 'TransInputTestHost.exe'
$tiProcess=Start-Process -FilePath $tiExe -ArgumentList '--self-check' -WorkingDirectory $tiDirectory -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $tiDirectory 'selfcheck.stdout.txt') -RedirectStandardError (Join-Path $tiDirectory 'selfcheck.stderr.txt') -PassThru
$tiHandle=$tiProcess.Handle
if(-not $tiProcess.WaitForExit(15000)){& taskkill.exe /PID $tiProcess.Id /T /F | Out-Null;throw "Copy self-check timed out: $tiDirectory"}
$tiProcess.WaitForExit()
if($null -eq $tiProcess.ExitCode -or $tiProcess.ExitCode -ne 0){Get-Content (Join-Path $tiDirectory 'selfcheck.stderr.txt');throw "Copy self-check failed (handle $tiHandle). No interactive test opened."}
if($NoOpen){Write-Output "Checked isolated test copy: $tiDirectory";exit 0}
$tiManual=Start-Process -FilePath $tiExe -WorkingDirectory $tiDirectory -WindowStyle Normal `
    -RedirectStandardOutput (Join-Path $tiDirectory 'manual.stdout.txt') -RedirectStandardError (Join-Path $tiDirectory 'manual.stderr.txt') -PassThru
[ordered]@{path=$tiDirectory;pid=$tiManual.Id;architecture=$Architecture;opened_at=(Get-Date).ToString('o')} |
    ConvertTo-Json | Set-Content (Join-Path $tiRoot 'artifacts\manual-input\active.json') -Encoding UTF8
if($tiManual.WaitForExit(3000)){Get-Content (Join-Path $tiDirectory 'manual.stderr.txt');throw 'Interactive native editor exited during startup.'}
Write-Output "Opened own input test: PID $($tiManual.Id), $tiDirectory"

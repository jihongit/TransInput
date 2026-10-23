# SPDX-License-Identifier: AGPL-3.0-only
# Publishes only the reviewed, isolated export, never the parent working folder.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SourceDirectory)
$ErrorActionPreference='Stop'
$tiRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiSource=[IO.Path]::GetFullPath($SourceDirectory)
$tiPrefix=[IO.Path]::GetFullPath((Join-Path $tiRoot 'artifacts\source-export'))+[IO.Path]::DirectorySeparatorChar
if(-not $tiSource.StartsWith($tiPrefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Publish only a source-export artifact.'}
$tiManifestPath=Join-Path (Split-Path -Parent $tiSource) 'source-manifest.json'
$tiManifest=Get-Content -LiteralPath $tiManifestPath -Raw|ConvertFrom-Json
foreach($tiItem in $tiManifest.files){
    $tiPath=[IO.Path]::GetFullPath((Join-Path $tiSource $tiItem.path))
    if(-not $tiPath.StartsWith($tiSource+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
        (Get-FileHash -LiteralPath $tiPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $tiItem.sha256){throw 'Export hash/path validation failed.'}
}
$tiActual=@(Get-ChildItem -LiteralPath $tiSource -Recurse -File -Force|Where-Object {$_.FullName -notlike ($tiSource+'\.git\*')})
if($tiActual.Count -ne $tiManifest.files.Count){throw 'Extra/missing export files: re-export before publishing.'}
$tiOldInteractive=$env:GCM_INTERACTIVE;$tiOldPrompt=$env:GIT_TERMINAL_PROMPT
$tiCredential=$null;$tiHeaders=$null
try{
    $env:GCM_INTERACTIVE='never';$env:GIT_TERMINAL_PROMPT='0'
    # Credential output remains in this process. Never write it to a file/log.
    $tiCredential=@("protocol=https`nhost=github.com`nusername=jihongit`n`n"|git credential fill 2>$null)
    if($LASTEXITCODE -ne 0){throw 'GitHub authentication required. In your local terminal run: git credential-manager github login --username jihongit --device --no-ui'}
    $tiPasswordLine=$tiCredential|Where-Object {$_.StartsWith('password=')}|Select-Object -First 1
    if(-not $tiPasswordLine){throw 'GitHub credential unavailable; authenticate locally first.'}
    $tiHeaders=@{Authorization=('Bearer '+$tiPasswordLine.Substring(9));'User-Agent'='TransInput-release';Accept='application/vnd.github+json';'X-GitHub-Api-Version'='2022-11-28'}
    $tiUser=Invoke-RestMethod -Uri 'https://api.github.com/user' -Headers $tiHeaders -TimeoutSec 30
    if($tiUser.login -cne 'jihongit'){throw 'Authenticated owner is not jihongit; nothing published.'}
    $tiRepository=$null
    try{$tiRepository=Invoke-RestMethod -Uri 'https://api.github.com/repos/jihongit/TransInput' -Headers $tiHeaders -TimeoutSec 30}
    catch{if([int]$_.Exception.Response.StatusCode -ne 404){throw 'Repository lookup failed; nothing pushed.'}}
    if(-not $tiRepository){
        $tiBody=@{name='TransInput';private=$true;auto_init=$false;has_issues=$true;description='Independent Windows Chinese input and optional DeepSeek translation; Rust core and native TSF.'}|ConvertTo-Json
        $tiRepository=Invoke-RestMethod -Method Post -Uri 'https://api.github.com/user/repos' -Headers $tiHeaders -ContentType 'application/json' -Body $tiBody -TimeoutSec 30
    }
    if($tiRepository.full_name -cne 'jihongit/TransInput' -or -not $tiRepository.private){throw 'Repository must remain private until explicit user acceptance; no push performed.'}
    $tiRemote='https://jihongit@github.com/jihongit/TransInput.git'
    $tiRefs=@(git -c credential.username=jihongit ls-remote $tiRemote)
    if($LASTEXITCODE -ne 0){throw 'Cannot verify remote repository state.'}
    if($tiRefs.Count){throw 'Repository is not empty; existing history will not be overwritten.'}
    if(Test-Path -LiteralPath (Join-Path $tiSource '.git')){throw 'Export already contains Git state; create a fresh export.'}
    git init -b main $tiSource|Out-Null;if($LASTEXITCODE -ne 0){throw 'Git init failed.'}
    git -C $tiSource add --all;if($LASTEXITCODE -ne 0){throw 'Git staging failed.'}
    $tiEmail=([string]$tiUser.id)+'+jihongit@users.noreply.github.com'
    git -C $tiSource -c user.name=jihongit -c user.email=$tiEmail commit -m 'Initial independent TransInput Windows preview'|Out-Null
    if($LASTEXITCODE -ne 0){throw 'Git commit failed.'}
    git -C $tiSource remote add origin $tiRemote;if($LASTEXITCODE -ne 0){throw 'Git remote setup failed.'}
    git -C $tiSource -c credential.username=jihongit push --set-upstream origin main
    if($LASTEXITCODE -ne 0){throw 'Push failed; local export and commit preserved.'}
    $tiCommit=(git -C $tiSource rev-parse HEAD).Trim()
    [ordered]@{repository=$tiRepository.html_url;private=$true;commit=$tiCommit;source=$tiSource;file_count=$tiManifest.files.Count;published_at=(Get-Date).ToUniversalTime().ToString('o')}|
        ConvertTo-Json|Set-Content (Join-Path (Split-Path -Parent $tiSource) 'publication.json') -Encoding UTF8
    Write-Output ('Uploaded private source: '+$tiRepository.html_url+' @ '+$tiCommit)
}finally{
    $tiHeaders=$null;$tiCredential=$null;$tiPasswordLine=$null
    $env:GCM_INTERACTIVE=$tiOldInteractive;$env:GIT_TERMINAL_PROMPT=$tiOldPrompt
}

# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param([string]$Destination)
$ErrorActionPreference='Stop'
$tiRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $Destination){$Destination=Join-Path $tiRoot ('artifacts\source-export\'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'\TransInput')}
$tiOutput=[IO.Path]::GetFullPath($Destination)
$tiAllowed=[IO.Path]::GetFullPath((Join-Path $tiRoot 'artifacts'))+[IO.Path]::DirectorySeparatorChar
if(-not $tiOutput.StartsWith($tiAllowed,[StringComparison]::OrdinalIgnoreCase)){throw 'Source export must be inside this workspace artifacts directory.'}
if(Test-Path -LiteralPath $tiOutput){throw 'Export destination already exists; nothing overwritten.'}
# Reject directory junctions before traversal or creation, including the output's
# existing parents. A lexical workspace prefix alone does not constrain junctions.
$tiParent=Split-Path -Parent $tiOutput
while($tiParent -and $tiParent.Length -ge $tiRoot.Length){
    if(Test-Path -LiteralPath $tiParent){if((Get-Item -LiteralPath $tiParent -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Export parent cannot be a reparse point.'}}
    $tiParent=Split-Path -Parent $tiParent
}
function Get-TiSourceFiles([string]$Path){
    $tiItem=Get-Item -LiteralPath $Path -Force
    if($tiItem.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Source reparse point rejected.'}
    if(-not $tiItem.PSIsContainer){return $tiItem}
    foreach($tiChild in Get-ChildItem -LiteralPath $Path -Force){Get-TiSourceFiles $tiChild.FullName}
}
$tiNames=@('.github','assets','bindings','crates','docs','platforms','prompts','scripts','sources','.gitattributes','.gitignore','AGENTS.md','Cargo.toml','Cargo.lock','README.md','LICENSE','NOTICE','PRIVACY.md','SECURITY.md','CONTRIBUTING.md')
$tiFiles=[Collections.Generic.List[object]]::new()
foreach($tiName in $tiNames){
    $tiPath=Join-Path $tiRoot $tiName
    foreach($tiFile in @(Get-TiSourceFiles $tiPath)){
        $tiRelative=$tiFile.FullName.Substring($tiRoot.Length+1).Replace('\','/')
        if($tiFile.Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Reparse point rejected: $tiRelative"}
        if($tiFile.Extension -match '^\.(pfx|p12|key|exe|dll|bin|dmp|log|obj|pdb|user|suo)$' -or $tiFile.Name -match '^\.env($|\.)' -or $tiFile.Name -match '^(settings|learning|user-words)\.bin($|\.)'){throw "Private/build file rejected: $tiRelative"}
        if($tiFile.Extension -notin @('.png','.ico')){
            if($tiFile.Length -gt 32MB){throw "Unexpected large source file: $tiRelative"}
            $tiText=[IO.File]::ReadAllText($tiFile.FullName)
            if($tiText -match '(?m)^-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----\s*$' -or $tiText -match '\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{60,}|sk-[A-Za-z0-9]{24,})\b'){throw "Possible secret rejected; inspect locally: $tiRelative"}
        }
        $tiFiles.Add([pscustomobject]@{file=$tiFile;relative=$tiRelative;sha256=(Get-FileHash -LiteralPath $tiFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()})
    }
}
New-Item -ItemType Directory -Path $tiOutput -Force|Out-Null
foreach($tiEntry in $tiFiles){
    $tiTarget=Join-Path $tiOutput $tiEntry.relative
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($tiTarget)) -Force|Out-Null
    Copy-Item -LiteralPath $tiEntry.file.FullName -Destination $tiTarget
}
$tiManifest=@($tiFiles|Sort-Object relative|ForEach-Object {[ordered]@{path=$_.relative;sha256=$_.sha256}})
$tiEvidence=Split-Path -Parent $tiOutput
[ordered]@{schema=1;files=$tiManifest;secret_scan='known credential/private-key patterns; not proof of absence of every secret';parent_project_included=$false}|
    ConvertTo-Json -Depth 5|Set-Content (Join-Path $tiEvidence 'source-manifest.json') -Encoding UTF8
[ordered]@{path=$tiOutput;files=$tiFiles.Count;manifest=(Join-Path $tiEvidence 'source-manifest.json')}|ConvertTo-Json

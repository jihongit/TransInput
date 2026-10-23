# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param([ValidateSet('preview','release')][string]$Channel='preview')
$ErrorActionPreference='Stop'
$tiRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiLatest=Get-Content -LiteralPath (Join-Path $tiRoot 'artifacts\p1\latest-run.json') -Raw|ConvertFrom-Json
if($tiLatest.status -ne 'passed' -or $tiLatest.scope -ne 'all'){throw 'A passing complete dual-architecture verification is required.'}
if(@($tiLatest.architectures).Count -ne 2 -or ((@($tiLatest.architectures|Sort-Object -Unique) -join ',') -cne 'x64,x86')){throw 'Both x64 and x86 must be verified in the same complete run.'}
$tiSnapshot=Get-Content -LiteralPath (Join-Path $tiLatest.path 'source-snapshot.json') -Raw|ConvertFrom-Json
$tiBinaries=Get-Content -LiteralPath (Join-Path $tiLatest.path 'binary-snapshot.json') -Raw|ConvertFrom-Json
foreach($tiSource in $tiSnapshot.sources){
    if((Get-FileHash -LiteralPath (Join-Path $tiRoot $tiSource.path) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $tiSource.sha256){throw ('Source changed after verification: '+$tiSource.path)}
}
$tiStamp=Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$tiEvidence=Join-Path $tiRoot ('artifacts\delivery\'+$tiStamp)
$tiPackage=Join-Path $tiEvidence ('TransInput-0.1.0-'+$Channel+'-windows')
$tiMap=[ordered]@{
    'x64/TransInputPrototype.dll'='build/x64/TransInputPrototype.dll'
    'x86/TransInputPrototype.dll'='build/x86/TransInputPrototype.dll'
    'x64/TransInputRegister.exe'='build/x64/TransInputRegister.exe'
    'x86/TransInputRegister.exe'='build/x86/TransInputRegister.exe'
    'shared/TransInputBroker.exe'='build/shared/TransInputBroker.exe'
    'shared/TransInputTranslator.exe'='build/shared/TransInputTranslator.exe'
    'shared/TransInputSettings.exe'='build/shared/TransInputSettings.exe'
    'shared/transinput_core.dll'='build/shared/transinput_core.dll'
    'shared/transinput-icon.png'='assets/branding/original/transinput-icon.png'
    'tools/TransInputPreflight.exe'='build/x64/TransInputPreflight.exe'
    'LICENSE'='LICENSE';'NOTICE'='NOTICE';'PRIVACY.md'='PRIVACY.md'
}
$tiSignatures=@();$tiManifest=@()
foreach($tiPair in $tiMap.GetEnumerator()){
    $tiSource=Join-Path $tiRoot $tiPair.Value
    if(-not (Test-Path -LiteralPath $tiSource -PathType Leaf)){throw ('Missing package input: '+$tiPair.Value)}
    if([IO.Path]::GetExtension($tiSource) -in @('.dll','.exe')){
        $tiVerified=@($tiBinaries.files|Where-Object {$_.path -ceq $tiPair.Value})
        if($tiVerified.Count -ne 1 -or (Get-FileHash -LiteralPath $tiSource -Algorithm SHA256).Hash.ToLowerInvariant() -ne $tiVerified[0].sha256){throw ('Binary changed after verification: '+$tiPair.Value)}
        $tiSignature=Get-AuthenticodeSignature -LiteralPath $tiSource
        $tiSignatures+=[ordered]@{path=$tiPair.Key;status=$tiSignature.Status.ToString()}
        if($Channel -eq 'release' -and $tiSignature.Status -ne 'Valid'){throw ('Trusted code signature required: '+$tiPair.Key)}
    }
}
if($Channel -eq 'release'){throw 'Release gate: persistent installation and actual-host acceptance are not yet signed off. Preview export remains available.'}
New-Item -ItemType Directory -Path $tiPackage -Force|Out-Null
foreach($tiPair in $tiMap.GetEnumerator()){
    $tiTarget=Join-Path $tiPackage $tiPair.Key
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($tiTarget)) -Force|Out-Null
    Copy-Item -LiteralPath (Join-Path $tiRoot $tiPair.Value) -Destination $tiTarget
}
foreach($tiNotice in @('cppwinrt','rust-runtime','pinyin-simp')){
    $tiTarget=Join-Path $tiPackage ('sources/'+$tiNotice)
    New-Item -ItemType Directory -Path $tiTarget -Force|Out-Null
    # Preserve complete reviewed provenance/licenses, not a rewritten license summary.
    Copy-Item -LiteralPath (Join-Path $tiRoot ('sources/'+$tiNotice)) -Destination (Join-Path $tiPackage 'sources') -Recurse -Force
}
$tiReadme=@'
# TransInput Windows development preview

This archive is unsigned and is not an installed Windows input method.
No registry, keyboard default, startup entry, trust store or Start Menu changes
are performed by unpacking it. Do not deploy it as a finished IME.

Current-account, read-only deployment check:
  tools\TransInputPreflight.exe --probe x64\TransInputPrototype.dll

The diagnostic JSON's installed field remains false. Signature and machine
registration permissions are separate prerequisites. Never bypass company policy,
trust unverified certificates, or turn off Windows protection to load this preview.

Source: https://github.com/jihongit/TransInput (intended repository; publication
status is recorded separately). See the matching source snapshot and NOTICE.
API keys, personal input and learning files are excluded.
'@
[IO.File]::WriteAllText((Join-Path $tiPackage 'README-FIRST.md'),$tiReadme,[Text.UTF8Encoding]::new($false))
foreach($tiFile in Get-ChildItem -LiteralPath $tiPackage -File -Recurse){$tiManifest+=[ordered]@{path=$tiFile.FullName.Substring($tiPackage.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $tiFile.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}}
[ordered]@{schema=1;channel=$Channel;installed=$false;verification=$tiLatest.path;files=@($tiManifest|Sort-Object path);signatures=$tiSignatures}|
    ConvertTo-Json -Depth 6|Set-Content (Join-Path $tiPackage 'manifest.json') -Encoding UTF8
Compress-Archive -LiteralPath $tiPackage -DestinationPath ($tiPackage+'.zip') -CompressionLevel Optimal
$tiHash=(Get-FileHash -LiteralPath ($tiPackage+'.zip') -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText(($tiPackage+'.zip.sha256'),($tiHash+'  '+[IO.Path]::GetFileName($tiPackage)+'.zip'+[Environment]::NewLine),[Text.UTF8Encoding]::new($false))
[ordered]@{path=$tiPackage;archive=($tiPackage+'.zip');sha256=$tiHash;channel=$Channel;installed=$false}|ConvertTo-Json

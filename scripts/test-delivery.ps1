# SPDX-License-Identifier: AGPL-3.0-only
# Fixed fictional packaging/export fixtures, all below artifacts; no network.
$ErrorActionPreference='Stop'
$tiProject=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiEvidence=Join-Path $tiProject ('artifacts/delivery-tests/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $tiEvidence -Force|Out-Null
$tiResults=[Collections.Generic.List[object]]::new()
function New-Fixture([string]$Name){
    $tiFixture=Join-Path $tiEvidence $Name
    foreach($tiName in @('scripts','.github','assets','bindings','crates','docs','platforms','prompts','sources','build/x64','build/x86','build/shared','artifacts/p1/checkpoint','assets/branding/original','sources/cppwinrt','sources/rust-runtime','sources/pinyin-simp')){New-Item -ItemType Directory -Path (Join-Path $tiFixture $tiName) -Force|Out-Null}
    foreach($tiFile in @('.gitattributes','.gitignore','AGENTS.md','Cargo.toml','Cargo.lock','README.md','LICENSE','NOTICE','PRIVACY.md','SECURITY.md','CONTRIBUTING.md')){[IO.File]::WriteAllText((Join-Path $tiFixture $tiFile),'fixed fictional source')}
    foreach($tiScript in @('export-source.ps1','package-windows.ps1')){Copy-Item -LiteralPath (Join-Path $tiProject ('scripts/'+$tiScript)) -Destination (Join-Path $tiFixture ('scripts/'+$tiScript))}
    [IO.File]::WriteAllText((Join-Path $tiFixture 'crates/learning.rs'),'// fixed authored source, not private learning data')
    [IO.File]::WriteAllText((Join-Path $tiFixture 'assets/branding/original/transinput-icon.png'),'fixed fixture, not a rendered image')
    foreach($tiName in @('cppwinrt','rust-runtime','pinyin-simp')){[IO.File]::WriteAllText((Join-Path $tiFixture ('sources/'+$tiName+'/LICENSE')),'fixed license fixture')}
    foreach($tiFile in @('build/x64/TransInputPrototype.dll','build/x86/TransInputPrototype.dll','build/x64/TransInputRegister.exe','build/x86/TransInputRegister.exe','build/shared/TransInputBroker.exe','build/shared/TransInputTranslator.exe','build/shared/TransInputSettings.exe','build/shared/transinput_core.dll','build/x64/TransInputPreflight.exe')){[IO.File]::WriteAllText((Join-Path $tiFixture $tiFile),'fixed inert executable fixture, never run')}
    $tiSources=@('Cargo.toml','assets/branding/original/transinput-icon.png')|ForEach-Object {[ordered]@{path=$_;sha256=(Get-FileHash (Join-Path $tiFixture $_)).Hash.ToLowerInvariant()}}
    [ordered]@{sources=@($tiSources)}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $tiFixture 'artifacts/p1/checkpoint/source-snapshot.json') -Encoding UTF8
    $tiBinaries=Get-ChildItem (Join-Path $tiFixture 'build') -Recurse -File|ForEach-Object {[ordered]@{path=$_.FullName.Substring($tiFixture.Length+1).Replace('\','/');sha256=(Get-FileHash $_.FullName).Hash.ToLowerInvariant()}}
    [ordered]@{files=@($tiBinaries)}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $tiFixture 'artifacts/p1/checkpoint/binary-snapshot.json') -Encoding UTF8
    [ordered]@{path=(Join-Path $tiFixture 'artifacts/p1/checkpoint');status='passed';scope='all';architectures=@('x64','x86')}|ConvertTo-Json|Set-Content (Join-Path $tiFixture 'artifacts/p1/latest-run.json') -Encoding UTF8
    return $tiFixture
}
function Run-Case([string]$Name,[string]$Fixture,[string]$Script,[string[]]$Arguments,[bool]$Success){
    $tiOut=Join-Path $tiEvidence ($Name+'.stdout.txt');$tiErr=Join-Path $tiEvidence ($Name+'.stderr.txt')
    $tiArgs=@('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+(Join-Path $Fixture ('scripts/'+$Script))+'"'))+$Arguments
    $tiProcess=Start-Process powershell.exe -ArgumentList $tiArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput $tiOut -RedirectStandardError $tiErr
    $tiHandle=$tiProcess.Handle;if(-not $tiProcess.WaitForExit(25000)){& taskkill.exe /PID $tiProcess.Id /T /F|Out-Null;throw 'Owned script fixture timed out'};$tiProcess.WaitForExit()
    $tiPassed=($tiProcess.ExitCode -eq 0) -eq $Success
    $tiResults.Add([ordered]@{name=$Name;exit_code=$tiProcess.ExitCode;expected_success=$Success;passed=$tiPassed})
    $tiResults|ConvertTo-Json -Depth 4|Set-Content (Join-Path $tiEvidence 'checks.json') -Encoding UTF8
    if(-not $tiPassed){Get-Content $tiOut;Get-Content $tiErr;throw ($Name+' did not satisfy expected result')}
    Write-Output ($Name+': PASS')
}
$tiFixture=New-Fixture 'export-good'
New-Item -ItemType Directory -Path (Join-Path $tiFixture '.tools')|Out-Null
[IO.File]::WriteAllText((Join-Path $tiFixture '.tools/never-publish.key'),'fixed excluded marker')
Run-Case 'export-allowlist' $tiFixture 'export-source.ps1' @() $true
$tiExport=Get-ChildItem (Join-Path $tiFixture 'artifacts/source-export') -Directory|Select-Object -First 1
if(-not (Test-Path (Join-Path $tiExport.FullName 'TransInput/crates/learning.rs')) -or (Test-Path (Join-Path $tiExport.FullName 'TransInput/.tools'))){throw 'Source inclusion/exclusion mismatch'}
Run-Case 'export-outside-denied' $tiFixture 'export-source.ps1' @('-Destination',('"'+(Join-Path $tiFixture 'outside')+'"')) $false
$tiFixture=New-Fixture 'export-secret';[IO.File]::WriteAllText((Join-Path $tiFixture 'docs/fixed-secret.txt'),('sk-'+('a'*32)))
Run-Case 'export-fixed-secret-rejected' $tiFixture 'export-source.ps1' @() $false
$tiFixture=New-Fixture 'export-private';[IO.File]::WriteAllText((Join-Path $tiFixture 'crates/learning.bin.settings'),'fixed private marker')
Run-Case 'export-private-profile-rejected' $tiFixture 'export-source.ps1' @() $false
$tiFixture=New-Fixture 'package-good';[IO.File]::WriteAllText((Join-Path $tiFixture 'build/shared/settings.bin'),'fixed private marker')
Run-Case 'preview-package' $tiFixture 'package-windows.ps1' @('-Channel','preview') $true
$tiZip=Get-ChildItem (Join-Path $tiFixture 'artifacts/delivery') -Recurse -Filter '*.zip'|Select-Object -First 1
if(-not $tiZip){throw 'Preview archive missing'}
$tiManifest=Get-ChildItem (Join-Path $tiFixture 'artifacts/delivery') -Recurse -Filter 'manifest.json'|Select-Object -First 1
$tiPack=Get-Content $tiManifest.FullName -Raw|ConvertFrom-Json
if(@($tiPack.files|Where-Object path -like '*settings.bin*').Count -or $tiPack.installed){throw 'Private data included or install misrepresented'}
Run-Case 'unsigned-release-denied' $tiFixture 'package-windows.ps1' @('-Channel','release') $false
$tiFixture=New-Fixture 'package-source-change';[IO.File]::AppendAllText((Join-Path $tiFixture 'Cargo.toml'),'changed')
Run-Case 'changed-source-denied' $tiFixture 'package-windows.ps1' @() $false
$tiFixture=New-Fixture 'package-binary-change';[IO.File]::AppendAllText((Join-Path $tiFixture 'build/shared/transinput_core.dll'),'changed')
Run-Case 'changed-binary-denied' $tiFixture 'package-windows.ps1' @() $false
$tiFixture=New-Fixture 'package-single-architecture'
$tiLatest=Get-Content (Join-Path $tiFixture 'artifacts/p1/latest-run.json') -Raw|ConvertFrom-Json
$tiLatest.architectures=@('x64')
$tiLatest|ConvertTo-Json|Set-Content (Join-Path $tiFixture 'artifacts/p1/latest-run.json') -Encoding UTF8
Run-Case 'single-architecture-denied' $tiFixture 'package-windows.ps1' @() $false
Write-Output ('Evidence: '+$tiEvidence)

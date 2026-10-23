# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'all')][string]$Architecture = 'all',
    [ValidateSet('all','native-input')][string]$Scope = 'all'
)
$ErrorActionPreference = 'Stop'
$tiRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiEvidenceRoot = Join-Path $tiRoot 'artifacts\p1'
$tiEvidence = Join-Path $tiEvidenceRoot (Get-Date -Format 'yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Force $tiEvidence | Out-Null
[ordered]@{path=$tiEvidence; scope=$Scope; status='running'} | ConvertTo-Json |
    Set-Content (Join-Path $tiEvidenceRoot 'latest-run.json') -Encoding UTF8
$tiResults = [Collections.Generic.List[object]]::new()
$tiArchitectures = if ($Architecture -eq 'all') { @('x64', 'x86') } else { @($Architecture) }
trap {
    [ordered]@{path=$tiEvidence;scope=$Scope;status='failed';check=$script:tiCurrentCheck;error=$_.Exception.GetType().FullName}|ConvertTo-Json|
        Set-Content (Join-Path $tiEvidenceRoot 'latest-run.json') -Encoding UTF8
    throw
}

function Invoke-TiCheck([string]$Name, [string]$Executable, [string[]]$Arguments, [int]$Timeout = 90) {
    $script:tiCurrentCheck=$Name
    $tiStdout = Join-Path $tiEvidence ($Name + '.stdout.txt')
    $tiStderr = Join-Path $tiEvidence ($Name + '.stderr.txt')
    $tiClock = [Diagnostics.Stopwatch]::StartNew()
    $tiStart=@{FilePath=$Executable;WorkingDirectory=$tiRoot;WindowStyle='Hidden';RedirectStandardOutput=$tiStdout;RedirectStandardError=$tiStderr;PassThru=$true}
    if($Arguments.Count){$tiStart.ArgumentList=$Arguments}
    $tiProcess = Start-Process @tiStart
    # Keep the Windows process handle open even when a short check exits before
    # WaitForExit; Windows PowerShell otherwise sometimes returns a null exit code.
    $tiProcessHandle = $tiProcess.Handle
    if (-not $tiProcess.WaitForExit($Timeout * 1000)) {
        # Kill only this test/build process and its children, never other IME processes.
        & taskkill.exe /PID $tiProcess.Id /T /F | Out-Null
        throw "$Name exceeded ${Timeout}s; see $tiEvidence"
    }
    $tiProcess.WaitForExit()
    if ($null -eq $tiProcess.ExitCode) { throw "$Name did not return an exit code (handle $tiProcessHandle)" }
    $tiClock.Stop()
    $tiRecord = [ordered]@{name=$Name; exit_code=$tiProcess.ExitCode; elapsed_ms=$tiClock.ElapsedMilliseconds}
    $tiResults.Add($tiRecord)
    $tiResults | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $tiEvidence 'checks.json') -Encoding UTF8
    Write-Output "$Name : exit $($tiProcess.ExitCode), $($tiClock.ElapsedMilliseconds)ms"
    if ($tiProcess.ExitCode -ne 0) {
        [ordered]@{path=$tiEvidence;scope=$Scope;status='failed';check=$Name} | ConvertTo-Json |
            Set-Content (Join-Path $tiEvidenceRoot 'latest-run.json') -Encoding UTF8
        Get-Content $tiStdout
        Get-Content $tiStderr
        throw "$Name failed"
    }
}

Invoke-TiCheck 'dictionary-integrity' 'powershell.exe' @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSScriptRoot\prepare-dictionary.ps1`"", '-VerifyOnly') 20
if($Scope -eq 'all') { Invoke-TiCheck 'delivery-script-fixtures' 'powershell.exe' @('-NoProfile','-File',('"'+$PSScriptRoot+'\test-delivery.ps1"')) 90 }

foreach ($tiArch in $tiArchitectures) {
    if($Scope -eq 'all') {
        foreach ($tiAction in @('fmt', 'check', 'test', 'build')) {
            Invoke-TiCheck "$tiArch-rust-$tiAction" $env:ComSpec @('/d', '/c', "`"`"$PSScriptRoot\msvc.cmd`" $tiArch cargo $tiAction`"")
        }
    }
    $tiNativeMode = if($Scope -eq 'native-input') { ' tsf' } else { '' }
    if($Scope -eq 'native-input') {
        foreach($tiRequired in @("build\$tiArch\TransInputTranslationFixture.exe", 'build\shared\TransInputBroker.exe')) {
            if(-not (Test-Path -LiteralPath (Join-Path $tiRoot $tiRequired))) { throw 'native-input requires a previous complete build; run with -Scope all first' }
        }
    }
    Invoke-TiCheck "$tiArch-native-build" $env:ComSpec @('/d', '/c', "`"`"$PSScriptRoot\msvc.cmd`" $tiArch native$tiNativeMode`"")
    $tiNative = Join-Path $tiRoot "build\$tiArch"
    $tiRust = if ($tiArch -eq 'x86') { Join-Path $tiRoot 'target\i686-pc-windows-msvc\release' } else { Join-Path $tiRoot 'target\release' }
    if($Scope -eq 'all') {
    Invoke-TiCheck "$tiArch-deployment-preflight-selfcheck" (Join-Path $tiNative 'TransInputPreflight.exe') @('--self-check') 10
    Invoke-TiCheck "$tiArch-deployment-probe" (Join-Path $tiNative 'TransInputPreflight.exe') @('--probe',('"'+(Join-Path $tiNative 'TransInputPrototype.dll')+'"')) 10
    Invoke-TiCheck "$tiArch-registration-transaction" (Join-Path $tiNative 'TransInputRegister.exe') @('--self-check') 10
    Invoke-TiCheck "$tiArch-registration-plan" (Join-Path $tiNative 'TransInputRegister.exe') @('--plan') 10
    Invoke-TiCheck "$tiArch-local-preview-boundaries" 'powershell.exe' @('-NoProfile','-File',('"'+$PSScriptRoot+'\test-local-preview.ps1"'),'-Architecture',$tiArch) 20
    Invoke-TiCheck "$tiArch-spelling-benchmark" (Join-Path $tiRust 'transinput-cli.exe') @('--benchmark-spelling') 30
    Invoke-TiCheck "$tiArch-settings-layout" (Join-Path $tiNative 'TransInputSettings.exe') @('--render-preview', ('"' + (Join-Path $tiEvidence "$tiArch-settings.png") + '"')) 10
    Invoke-TiCheck "$tiArch-learning-layout" (Join-Path $tiNative 'TransInputSettings.exe') @('--render-learning-preview', ('"' + (Join-Path $tiEvidence "$tiArch-learning-settings.png") + '"')) 10
    Invoke-TiCheck "$tiArch-words-layout" (Join-Path $tiNative 'TransInputSettings.exe') @('--render-words-preview', ('"' + (Join-Path $tiEvidence "$tiArch-word-settings.png") + '"')) 10
    Invoke-TiCheck "$tiArch-words-settings" (Join-Path $tiNative 'TransInputSettingsTest.exe') @('--self-check-words') 20
    Invoke-TiCheck "$tiArch-c-abi" (Join-Path $tiNative 'abi-smoke.exe') @('"' + (Join-Path $tiRust 'transinput_core.dll') + '"') 20
    Invoke-TiCheck "$tiArch-learning" (Join-Path $tiNative 'learning-smoke.exe') @('"' + (Join-Path $tiRust 'transinput_core.dll') + '"') 25
    Invoke-TiCheck "$tiArch-words" (Join-Path $tiNative 'words-smoke.exe') @('"' + (Join-Path $tiRust 'transinput_core.dll') + '"') 35
    Invoke-TiCheck "$tiArch-translation" (Join-Path $tiNative 'translation-smoke.exe') @('"' + (Join-Path $tiRust 'transinput_core.dll') + '"') 30
    Invoke-TiCheck "$tiArch-translator-process" (Join-Path $tiNative 'translator-process.exe') @('"' + (Join-Path $tiNative 'TransInputTranslator.exe') + '"') 20
    Invoke-TiCheck "$tiArch-broker" (Join-Path $tiNative 'broker-smoke.exe') @('"' + (Join-Path $tiNative 'TransInputBroker.exe') + '"') 25
    }
    Invoke-TiCheck "$tiArch-com-lifecycle" (Join-Path $tiNative 'tsf-lifecycle.exe') @('"' + (Join-Path $tiNative 'TransInputPrototypeTest.dll') + '"') 20
    Invoke-TiCheck "$tiArch-production-com" (Join-Path $tiNative 'tsf-lifecycle.exe') @('"' + (Join-Path $tiNative 'TransInputPrototype.dll') + '"') 20
    Invoke-TiCheck "$tiArch-ui-element-contract" (Join-Path $tiNative 'ui-element-smoke.exe') @() 10
    Invoke-TiCheck "$tiArch-accessibility-contract" (Join-Path $tiNative 'accessibility-smoke.exe') @() 10
    $tiCandidatePreview = Join-Path $tiEvidence "$tiArch-candidates"
    New-Item -ItemType Directory -Force $tiCandidatePreview | Out-Null
    Invoke-TiCheck "$tiArch-candidate-layout" (Join-Path $tiNative 'candidate-preview.exe') @(('"' + (Join-Path $tiNative 'TransInputPrototypeTest.dll') + '"'), ('"' + $tiCandidatePreview + '"')) 10
    Invoke-TiCheck "$tiArch-tsf-context" (Join-Path $tiNative 'tsf-integration.exe') @('"' + (Join-Path $tiNative 'TransInputPrototypeTest.dll') + '"') 45
    Invoke-TiCheck "$tiArch-tsf-digits" (Join-Path $tiNative 'tsf-integration.exe') @(('"' + (Join-Path $tiNative 'TransInputPrototypeTest.dll') + '"'), '--digits') 35
    Invoke-TiCheck "$tiArch-tsf-pages" (Join-Path $tiNative 'tsf-integration.exe') @(('"' + (Join-Path $tiNative 'TransInputPrototypeTest.dll') + '"'), '--pages') 30
    Invoke-TiCheck "$tiArch-tsf-mixed" (Join-Path $tiNative 'tsf-integration.exe') @(('"' + (Join-Path $tiNative 'TransInputPrototypeTest.dll') + '"'), '--mixed') 45
    Invoke-TiCheck "$tiArch-tsf-words" (Join-Path $tiNative 'tsf-integration.exe') @(('"' + (Join-Path $tiNative 'TransInputPrototypeTest.dll') + '"'), '--words') 45
    Invoke-TiCheck "$tiArch-tsf-spelling" (Join-Path $tiNative 'tsf-integration.exe') @(('"' + (Join-Path $tiNative 'TransInputPrototypeTest.dll') + '"'), '--spelling') 45
    Invoke-TiCheck "$tiArch-tsf-uiless" (Join-Path $tiNative 'tsf-integration.exe') @(('"' + (Join-Path $tiNative 'TransInputPrototypeTest.dll') + '"'), '--uiless') 30
    Invoke-TiCheck "$tiArch-native-editor" (Join-Path $tiNative 'TransInputTestHost.exe') @('--self-check') 15
    Invoke-TiCheck "$tiArch-native-resources" (Join-Path $tiNative 'TransInputTestHost.exe') @('--soak-seconds=10') 30
    Invoke-TiCheck "$tiArch-isolated-copy" 'powershell.exe' @('-NoProfile','-File',('"'+$PSScriptRoot+'\open-input-test.ps1"'),'-Architecture',$tiArch,'-NoOpen') 20
}

$tiPaths = @('Cargo.toml','Cargo.lock','bindings','crates','platforms','prompts','scripts','sources/pinyin-simp','sources/cppwinrt','assets/branding/original','assets/branding/windows')
$tiSources = foreach ($tiPath in $tiPaths) {
    Get-ChildItem -LiteralPath (Join-Path $tiRoot $tiPath) -File -Recurse | ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($tiRoot.Length + 1).Replace('\','/'); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
}
[ordered]@{generated_at=(Get-Date).ToUniversalTime().ToString('o'); sources=@($tiSources | Sort-Object path)} |
    ConvertTo-Json -Depth 5 | Set-Content (Join-Path $tiEvidence 'source-snapshot.json') -Encoding UTF8
$tiBinaries=foreach($tiDirectory in @('build\x64','build\x86','build\shared')){
    Get-ChildItem -LiteralPath (Join-Path $tiRoot $tiDirectory) -File|Where-Object {$_.Extension -in @('.exe','.dll','.png')}|ForEach-Object {
        [ordered]@{path=$_.FullName.Substring($tiRoot.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
    }
}
[ordered]@{generated_at=(Get-Date).ToUniversalTime().ToString('o');files=@($tiBinaries|Sort-Object path)}|ConvertTo-Json -Depth 5|
    Set-Content (Join-Path $tiEvidence 'binary-snapshot.json') -Encoding UTF8
[ordered]@{path=$tiEvidence; scope=$Scope; status='passed';architectures=$tiArchitectures} | ConvertTo-Json |
    Set-Content (Join-Path $tiEvidenceRoot 'latest-run.json') -Encoding UTF8
Write-Output "Evidence: $tiEvidence"

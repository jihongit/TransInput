# SPDX-License-Identifier: AGPL-3.0-only
param([Parameter(Mandatory=$true)][string]$NativeDirectory,[Parameter(Mandatory=$true)][string]$CorePath)
$ErrorActionPreference='Stop'
$tiRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiNative=[IO.Path]::GetFullPath($NativeDirectory)
$tiCore=[IO.Path]::GetFullPath($CorePath)
if(-not $tiNative.StartsWith((Join-Path $tiRoot 'build\'),[StringComparison]::OrdinalIgnoreCase) -or
   -not $tiCore.StartsWith((Join-Path $tiRoot 'target\'),[StringComparison]::OrdinalIgnoreCase)){
    throw 'Shared broker inputs must be project build outputs.'
}
$tiShared=Join-Path $tiRoot 'build\shared'
New-Item -ItemType Directory -Force $tiShared | Out-Null
$tiClock=[Diagnostics.Stopwatch]::StartNew()
do {
    try {
        Copy-Item -LiteralPath (Join-Path $tiNative 'TransInputBroker.exe') -Destination (Join-Path $tiShared 'TransInputBroker.exe') -Force
        Copy-Item -LiteralPath (Join-Path $tiNative 'TransInputTranslator.exe') -Destination (Join-Path $tiShared 'TransInputTranslator.exe') -Force
        Copy-Item -LiteralPath (Join-Path $tiNative 'TransInputSettings.exe') -Destination (Join-Path $tiShared 'TransInputSettings.exe') -Force
        Copy-Item -LiteralPath (Join-Path $tiNative 'transinput-icon.png') -Destination (Join-Path $tiShared 'transinput-icon.png') -Force
        Copy-Item -LiteralPath $tiCore -Destination (Join-Path $tiShared 'transinput_core.dll') -Force
        exit 0
    }catch [IO.IOException]{
        # A prior isolated test can still hold these build files until the
        # broker's 15s idle exit. Never terminate an unrelated/live owner.
        if($tiClock.Elapsed.TotalSeconds -ge 20){throw 'Shared broker build files remain in use; no process was terminated.'}
        Start-Sleep -Milliseconds 500
    }
}while($true)

# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param([switch]$VerifyOnly)
$ErrorActionPreference = 'Stop'
$tiRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiSource = Join-Path $tiRoot 'sources\pinyin-simp\pinyin_simp.dict.yaml'
$tiOutput = Join-Path $tiRoot 'crates\transinput-core\data\base.tsv'
$tiExpected = 'e341598343a0f0f2035bb1aafc34a7f3bb7887deeecb3f60796262aaa2983e6b'
if (-not (Test-Path -LiteralPath $tiSource)) { throw 'Pinned upstream dictionary is missing.' }
if ((Get-FileHash -LiteralPath $tiSource -Algorithm SHA256).Hash -ine $tiExpected) {
    throw 'Upstream dictionary hash mismatch; conversion stopped.'
}
$tiEncoding = [Text.UTF8Encoding]::new($false, $true)
$tiLines = [IO.File]::ReadAllLines($tiSource, $tiEncoding)
$tiHeader = [Collections.Generic.List[string]]::new()
$tiData = [Text.StringBuilder]::new()
[void]$tiData.Append("# TransInput dictionary format v1; converted data, not engine source.`n")
[void]$tiData.Append("# Source: rime/rime-pinyin-simp @ 0c6861ef7420ee780270ca6d993d18d4101049d0`n")
[void]$tiData.Append("# Apache-2.0; upstream license and AOSP notice in sources/pinyin-simp/.`n")
$tiInData = $false
$tiRows = 0
foreach ($tiLine in $tiLines) {
    if (-not $tiInData) {
        if ($tiLine -eq '...') { $tiInData = $true; continue }
        if ($tiLine.Length -gt 0 -and -not $tiLine.StartsWith('#')) { $tiHeader.Add($tiLine) }
        continue
    }
    if ($tiLine.Length -eq 0) { continue }
    $tiFields = $tiLine.Split([char]9)
    [uint32]$tiWeight = 0
    if ($tiFields.Length -ne 3 -or $tiFields[0].Length -eq 0 -or
        $tiFields[0] -match '\p{Cc}' -or $tiEncoding.GetByteCount($tiFields[0]) -gt 512 -or
        $tiFields[1] -cnotmatch '^[a-z]+(?: [a-z]+)*$' -or
        $tiFields[1].Replace(' ', '').Length -gt 128 -or
        $tiFields[2] -notmatch '^[0-9]+$' -or -not [uint32]::TryParse($tiFields[2], [ref]$tiWeight)) {
        throw "Invalid source data row $($tiRows + 1); nothing was written."
    }
    [void]$tiData.Append($tiFields[0]).Append([char]9).Append($tiFields[1]).Append([char]9).Append($tiWeight).Append([char]10)
    ++$tiRows
}
if (($tiHeader -join "`n") -cne "---`nname: pinyin_simp`nversion: `"0.1`"`nsort: by_weight" -or
    -not $tiInData -or $tiRows -ne 65125) {
    throw 'Pinned header or row count changed; nothing was written.'
}
$tiCanonical = $tiData.ToString()
if ($VerifyOnly) {
    if (-not (Test-Path -LiteralPath $tiOutput) -or
        [IO.File]::ReadAllText($tiOutput, $tiEncoding) -cne $tiCanonical) {
        throw 'Bundled data differs from the verified source conversion.'
    }
} else {
    New-Item -ItemType Directory -Force (Split-Path $tiOutput) | Out-Null
    [IO.File]::WriteAllText($tiOutput, $tiCanonical, $tiEncoding)
}
[pscustomobject]@{rows=$tiRows; source_sha256=$tiExpected; converted_sha256=(Get-FileHash -LiteralPath $tiOutput -Algorithm SHA256).Hash.ToLowerInvariant()} |
    ConvertTo-Json

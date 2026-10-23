# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param([ValidateSet('official', 'rsproxy')][string]$Mirror = 'official')
$ErrorActionPreference = 'Stop'
$tiRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tiTools = Join-Path $tiRoot '.tools'
New-Item -ItemType Directory -Force $tiTools | Out-Null
$env:CARGO_HOME = Join-Path $tiTools 'cargo'
$env:RUSTUP_HOME = Join-Path $tiTools 'rustup'
$tiInstaller = Join-Path $tiTools 'rustup-init.exe'
$tiExpected = '6f4bef66261261fcb43131be8720bab817d403a09edec7455c371974b90bdb7e'
$tiVersion = '1.99.1'
if (-not (Test-Path -LiteralPath $tiInstaller)) {
    & curl.exe --fail --location --retry 2 --max-time 60 `
        'https://static.rust-lang.org/rustup/archive/1.29.1/x86_64-pc-windows-msvc/rustup-init.exe' --output $tiInstaller
    if ($LASTEXITCODE -ne 0) { throw 'Official rustup download failed; no installer was executed.' }
}
if ((Get-FileHash -LiteralPath $tiInstaller -Algorithm SHA256).Hash.ToLowerInvariant() -ne $tiExpected) {
    throw 'rustup SHA256 mismatch; no installer was executed.'
}
if ($Mirror -eq 'rsproxy') {
    # Trust the official version manifest; compare before downloading components.
    $tiOfficial = Join-Path $tiTools 'official-version.sha256'
    $tiManifest = Join-Path $tiTools 'mirror-version.toml'
    & curl.exe --fail --location --retry 1 --max-time 45 "https://static.rust-lang.org/dist/channel-rust-$tiVersion.toml.sha256" --output $tiOfficial
    if ($LASTEXITCODE -ne 0) { throw 'Cannot verify the official manifest hash.' }
    & curl.exe --fail --location --retry 1 --max-time 45 "https://rsproxy.cn/dist/channel-rust-$tiVersion.toml" --output $tiManifest
    if ($LASTEXITCODE -ne 0) { throw 'Mirror manifest download failed.' }
    $tiManifestExpected = ((Get-Content -LiteralPath $tiOfficial -Raw).Trim() -split '\s+')[0]
    if ((Get-FileHash -LiteralPath $tiManifest -Algorithm SHA256).Hash -ine $tiManifestExpected) {
        throw 'Mirror manifest does not match the official manifest.'
    }
    $env:RUSTUP_DIST_SERVER = 'https://rsproxy.cn'
    $env:RUSTUP_UPDATE_ROOT = 'https://rsproxy.cn/rustup'
} else {
    $env:RUSTUP_DIST_SERVER = 'https://static.rust-lang.org'
    $env:RUSTUP_UPDATE_ROOT = 'https://static.rust-lang.org/rustup'
}
& $tiInstaller -y --no-modify-path --default-host x86_64-pc-windows-msvc `
    --default-toolchain $tiVersion --profile minimal --component clippy,rustfmt
if ($LASTEXITCODE -ne 0) { throw 'Local Rust installation failed.' }
$env:RUSTUP_TOOLCHAIN = $tiVersion
& (Join-Path $env:CARGO_HOME 'bin\rustup.exe') target add i686-pc-windows-msvc
if ($LASTEXITCODE -ne 0) { throw '32-bit Rust standard library installation failed.' }
Set-Content -LiteralPath (Join-Path $tiTools 'toolchain-name.txt') -Value $tiVersion -Encoding ASCII
Write-Output 'Rust is ready in .tools; system PATH was not changed.'

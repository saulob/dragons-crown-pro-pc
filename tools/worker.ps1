# Deploys the lobby Worker (server\lobby) with Wrangler. Wrangler's own configuration decides where it goes.
#   .\tools\worker.ps1            npx wrangler deploy
#   .\tools\worker.ps1 -DryRun    npx wrangler deploy --dry-run (bundles, uploads nothing)
param([switch]$DryRun)

$ErrorActionPreference = 'Stop'

function Fail([string]$message) {
    [Console]::Error.WriteLine("worker: $message")
    exit 1
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$workerDir = Join-Path $repoRoot 'server\lobby'

if (-not (Test-Path -LiteralPath $workerDir -PathType Container)) { Fail "missing folder $workerDir" }
if (-not (Test-Path -LiteralPath (Join-Path $workerDir 'wrangler.toml') -PathType Leaf)) {
    Fail "missing $workerDir\wrangler.toml, the Wrangler configuration"
}
if (-not (Get-Command npx -ErrorAction SilentlyContinue)) { Fail 'npx not found; install Node.js (https://nodejs.org) and open a new terminal' }

$wrangler = @('wrangler', 'deploy')
if ($DryRun) { $wrangler += '--dry-run' }

Push-Location -LiteralPath $workerDir
try {
    & npx @wrangler
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -ne 0) {
    [Console]::Error.WriteLine("worker: wrangler exited with code $code")
    exit $code
}
Write-Host ($(if ($DryRun) { 'Worker bundle checked (dry run, nothing uploaded).' } else { 'Worker deployed.' }))

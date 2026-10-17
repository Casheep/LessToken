param([string]$CodexPath = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'resolve-codex.ps1')
$codexExecutable = Resolve-LessTokenCodex $CodexPath
& $codexExecutable plugin remove 'lesstoken@lesstoken-local'
if ($LASTEXITCODE) { throw 'Cannot remove LessToken plugin.' }
& $codexExecutable plugin marketplace remove 'lesstoken-local'
if ($LASTEXITCODE) { throw 'Cannot remove LessToken marketplace source.' }
Write-Output 'Removed plugin and its source. Saved originals remain under LocalAppData/LessToken; no personal files or other plugin settings were removed.'

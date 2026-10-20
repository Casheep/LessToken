param([switch]$WithoutHooks, [switch]$EnableFileWrites, [switch]$ReuseSnapshots, [switch]$ArchiveContext, [string]$DataDirectory = '', [string]$CodexPath = '')
$ErrorActionPreference = 'Stop'
if ($WithoutHooks -and ($ReuseSnapshots -or $ArchiveContext)) { throw 'ReuseSnapshots and ArchiveContext require Hooks.' }
$sourceRoot = Split-Path -Parent $PSScriptRoot
$binary = Join-Path $sourceRoot 'plugins/lesstoken/bin/lesstoken.exe'
if (-not (Test-Path -LiteralPath $binary)) { throw 'Run this script from the extracted Windows release package.' }
. (Join-Path $PSScriptRoot 'resolve-codex.ps1')
$codexExecutable = Resolve-LessTokenCodex $CodexPath
# Current Codex versions do not expand PLUGIN_ROOT in MCP command paths.
# Materialize this release's stable path; leave cwd unset so Codex supplies the project.
$mcpPath = Join-Path $sourceRoot 'plugins/lesstoken/.mcp.json'
$mcp = Get-Content -LiteralPath $mcpPath -Raw | ConvertFrom-Json
$mcp.mcpServers.lesstoken.command = (Resolve-Path -LiteralPath $binary).ProviderPath
$mcp.mcpServers.lesstoken.args = @('mcp')
if ($EnableFileWrites) { $mcp.mcpServers.lesstoken.args += '--allow-write' }
$runtimeEnv = @{}
if ($mcp.mcpServers.lesstoken.env) {
    foreach ($property in $mcp.mcpServers.lesstoken.env.PSObject.Properties) { $runtimeEnv[$property.Name] = $property.Value }
}
if ($DataDirectory) {
    $dataPath = [System.IO.Path]::GetFullPath($DataDirectory)
    $runtimeEnv['LESSTOKEN_DATA'] = $dataPath
}
$runtimeEnv['LESSTOKEN_REUSE_SNAPSHOTS'] = if ($WithoutHooks) { '0' } else { '1' }
$runtimeEnv['LESSTOKEN_CONTEXT_ARCHIVE'] = if ($ArchiveContext) { '1' } else { '0' }
$mcp.mcpServers.lesstoken | Add-Member -NotePropertyName env -NotePropertyValue $runtimeEnv -Force
[System.IO.File]::WriteAllText($mcpPath, ($mcp | ConvertTo-Json -Depth 20), (New-Object System.Text.UTF8Encoding($false)))
# Keep the extracted marketplace at a stable path; Codex caches the plugin on install.
# This script adds one named source and plugin via Codex's own commands. It does not
# replace AGENTS.md, edit credentials, or manufacture hook-trust records.
$manifest = Join-Path $sourceRoot 'plugins/lesstoken/.codex-plugin/plugin.json'
$data = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
if ($WithoutHooks) { $data.hooks = @() } else { $data.hooks = './hooks/hooks.json' }
$jsonText = $data | ConvertTo-Json -Depth 20
[System.IO.File]::WriteAllText($manifest, $jsonText, (New-Object System.Text.UTF8Encoding($false)))
& $codexExecutable plugin marketplace add $sourceRoot
if ($LASTEXITCODE) { throw 'Cannot add LessToken marketplace.' }
& $codexExecutable plugin add 'lesstoken@lesstoken-local'
if ($LASTEXITCODE) { throw 'Cannot install LessToken plugin.' }
Write-Output 'Installed. Start a new local Codex chat. Review the hook definition if Codex asks; installation does not trust hooks automatically.'
Write-Output 'MCP uses this extracted release path. Keep the folder in place. Hooks reduce supported log reads and test/build commands; other calls use the LessToken tools explicitly.'

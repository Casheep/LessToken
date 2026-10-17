function Resolve-LessTokenCodex([string]$ExplicitPath = '') {
    $candidates = @()
    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) { throw 'CodexPath must point to an existing Codex CLI executable.' }
        $candidates += (Resolve-Path -LiteralPath $ExplicitPath).ProviderPath
    } else {
        $command = Get-Command codex -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($command) { $candidates += $command.Source }
        if ($env:LOCALAPPDATA) {
            $desktopBin = Join-Path $env:LOCALAPPDATA 'OpenAI/Codex/bin'
            if (Test-Path -LiteralPath $desktopBin -PathType Container) {
                $candidates += Join-Path $desktopBin 'codex.exe'
                $candidates += @(Get-ChildItem -LiteralPath $desktopBin -Directory | Sort-Object LastWriteTimeUtc -Descending | ForEach-Object { Join-Path $_.FullName 'codex.exe' })
            }
        }
    }
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        return $candidate
    }
    throw 'Cannot locate Codex CLI. Open Codex desktop once, then retry; or pass -CodexPath with the full path to codex.exe.'
}

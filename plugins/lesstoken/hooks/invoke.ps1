param([ValidateSet('pre','post','prompt','context')][string]$Mode = 'post')
$ErrorActionPreference = 'Stop'
$exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin/lesstoken.exe'
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $exe
$start.Arguments = if ($Mode -eq 'post') { 'hook --post' } elseif ($Mode -eq 'prompt') { 'hook --prompt' } elseif ($Mode -eq 'context') { 'hook --context' } else { 'hook --rewrite-readonly' }
$start.WorkingDirectory = (Get-Location).ProviderPath
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
$start.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
# Match the MCP server's explicit data directory so hook references remain
# recoverable when installation used -DataDirectory. Never print config content.
$runtimeConfig = Join-Path (Split-Path -Parent $PSScriptRoot) '.mcp.json'
if (Test-Path -LiteralPath $runtimeConfig -PathType Leaf) {
    try {
        $runtime = Get-Content -LiteralPath $runtimeConfig -Raw | ConvertFrom-Json -ErrorAction Stop
        $configuredData = $runtime.mcpServers.lesstoken.env.LESSTOKEN_DATA
        if ($configuredData -is [string] -and -not [string]::IsNullOrWhiteSpace($configuredData)) {
            $start.EnvironmentVariables['LESSTOKEN_DATA'] = [System.IO.Path]::GetFullPath($configuredData)
        }
        foreach ($flag in @('LESSTOKEN_REUSE_SNAPSHOTS', 'LESSTOKEN_CONTEXT_ARCHIVE')) {
            $value = $runtime.mcpServers.lesstoken.env.$flag
            if ($value -eq '0' -or $value -eq '1') { $start.EnvironmentVariables[$flag] = [string]$value }
        }
    } catch { exit 0 }
}
if ($Mode -eq 'context' -and $start.EnvironmentVariables['LESSTOKEN_CONTEXT_ARCHIVE'] -ne '1') { exit 0 }
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $start
[void]$process.Start()
$stdout = $process.StandardOutput.ReadToEndAsync()
$stderr = $process.StandardError.ReadToEndAsync()
[Console]::OpenStandardInput().CopyTo($process.StandardInput.BaseStream)
$process.StandardInput.Close()
$process.WaitForExit()
$encoding = New-Object System.Text.UTF8Encoding($false)
$out = $encoding.GetBytes($stdout.GetAwaiter().GetResult())
[Console]::OpenStandardOutput().Write($out, 0, $out.Length)
$err = $encoding.GetBytes($stderr.GetAwaiter().GetResult())
[Console]::OpenStandardError().Write($err, 0, $err.Length)
exit $process.ExitCode

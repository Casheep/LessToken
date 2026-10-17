param([ValidateSet('pre','post')][string]$Mode = 'post')
$ErrorActionPreference = 'Stop'
$exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'bin/lesstoken.exe'
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $exe
$start.Arguments = if ($Mode -eq 'post') { 'hook --post' } else { 'hook --rewrite-readonly' }
$start.WorkingDirectory = (Get-Location).ProviderPath
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
$start.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
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

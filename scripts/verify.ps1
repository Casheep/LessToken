$ErrorActionPreference = 'Stop'
$packageRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $packageRoot 'plugins/lesstoken/bin/lesstoken.exe'
$version = (Get-Content -LiteralPath (Join-Path $packageRoot 'VERSION') -Raw).Trim()
$temporaryRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$fixture = Join-Path $temporaryRoot ('LessToken-check-' + [Guid]::NewGuid().ToString('N'))
$oldData = $env:LESSTOKEN_DATA
function Invoke-LocalRequest([string]$Mode, [string]$Request) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $exe
    $info.Arguments = $Mode + ' --root "' + $fixture + '"'
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $info
    try {
        [void]$process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.WriteLine($Request)
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(15000)) { $process.Kill(); throw 'Local self-check timed out.' }
        $text = $stdout.GetAwaiter().GetResult()
        $errorText = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode) { throw ('Local self-check failed: ' + $errorText) }
        return ($text | ConvertFrom-Json)
    } finally { $process.Dispose() }
}
try {
    New-Item -ItemType Directory -Path $fixture | Out-Null
    $env:LESSTOKEN_DATA = Join-Path $fixture 'state'
    $actualVersion = & $exe --version
    if ($LASTEXITCODE -or $actualVersion.Trim() -ne $version) { throw 'Executable version does not match package.' }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText((Join-Path $fixture 'rows.json'), '[{"id":1,"state":"pass"},{"id":2,"state":"fail"}]', $encoding)
    [System.IO.File]::WriteAllText((Join-Path $fixture 'build.log'), (('INFO ordinary build progress' + "`n") * 1000 + "ERROR CHECK-MARKER expected=3 actual=0`n"), $encoding)
    $tools = Invoke-LocalRequest 'mcp' '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
    if ($tools.result.tools.Count -ne 4 -or 'lt_batch' -in $tools.result.tools.name) { throw 'Default tool permissions do not match.' }
    $selected = Invoke-LocalRequest 'ops' '{"tool":"lt_result","arguments":{"action":"put","path":"rows.json","equals":{"state":"fail"},"fields":["id"]}}'
    if ($selected.items.Count -ne 1 -or $selected.items[0].id -ne 2 -or -not $selected.complete) { throw 'Direct JSON query failed.' }
    $request = @{ tool='lt_result'; arguments=@{ ref=$selected.ref; fields=@('id'); limit=2 } } | ConvertTo-Json -Depth 8 -Compress
    $page = Invoke-LocalRequest 'ops' $request
    if ($page.items.Count -ne 2) { throw 'Full table snapshot is unavailable.' }
    # lt_read returns text rather than JSON, so exercise it through the MCP envelope.
    $log = Invoke-LocalRequest 'mcp' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"lt_read","arguments":{"path":"build.log","kind":"log"}}}'
    if ($log.result.isError -or $log.result.content[0].text -notmatch 'CHECK-MARKER') { throw 'Log diagnostic was lost.' }
    [System.IO.File]::WriteAllText((Join-Path $fixture 'adjacent.txt'), "before`nCHECK expected=3`nCHECK actual=0`nafter", $encoding)
    $grouped = Invoke-LocalRequest 'ops' '{"tool":"lt_search","arguments":{"path":"adjacent.txt","query":"CHECK"}}'
    if (-not $grouped.complete -or $grouped.matching_lines -ne 2 -or $grouped.matches.Count -ne 1 -or ($grouped.matches[0].lines -join ',') -ne '2,3') { throw 'Search grouping lost matched positions.' }
    Write-Output ('PASS LessToken ' + $version + ': 4 default tools, direct query, full snapshot, log diagnostic and search grouping.')
    Write-Output 'Local self-check only. No model calls, installation, login changes, or hook trust changes.'
} finally {
    $env:LESSTOKEN_DATA = $oldData
    $resolvedFixture = [System.IO.Path]::GetFullPath($fixture)
    if ((Split-Path -Parent $resolvedFixture).TrimEnd('\') -eq $temporaryRoot.TrimEnd('\') -and (Split-Path -Leaf $resolvedFixture) -match '^LessToken-check-[a-f0-9]{32}$' -and (Test-Path -LiteralPath $resolvedFixture)) {
        Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
    }
}

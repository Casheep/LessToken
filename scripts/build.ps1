param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
$compiler = Get-Command g++ -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
$ninja = Get-Command ninja -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $cmake -or -not $compiler -or -not $ninja) {
    $clion = Get-ChildItem 'C:/Program Files/JetBrains' -Directory -Filter 'CLion*' -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
    if ($clion) {
        if (-not $cmake) { $cmake = Join-Path $clion.FullName 'bin/cmake/win/x64/bin/cmake.exe' }
        if (-not $compiler) { $compiler = Join-Path $clion.FullName 'bin/mingw/bin/g++.exe' }
        if (-not $ninja) { $ninja = Join-Path $clion.FullName 'bin/ninja/win/x64/ninja.exe' }
    }
}
foreach ($path in @($cmake, $compiler, $ninja)) { if (-not $path -or -not (Test-Path -LiteralPath $path)) { throw 'Install CMake, Ninja and a C++20 compiler or use CLion bundled tools.' } }
$oldPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $compiler) + ';' + $oldPath
    & $cmake -S $workspace -B "$workspace/build" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_CXX_COMPILER=$compiler" "-DCMAKE_BUILD_TYPE=$Configuration"
    if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
    & $cmake --build "$workspace/build" --parallel 2
    if ($LASTEXITCODE) { throw 'C++ build failed.' }
} finally { $env:PATH = $oldPath }
New-Item -ItemType Directory -Force "$workspace/plugins/lesstoken/bin" | Out-Null
Copy-Item -LiteralPath "$workspace/build/lesstoken.exe" -Destination "$workspace/plugins/lesstoken/bin/lesstoken.exe" -Force
Write-Output 'Build complete. The executable is ready for scripts/verify.ps1 and scripts/install.ps1.'

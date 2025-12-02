$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
function Make-TestPath([string]$child) {
    return (Join-Path $root $child)
}
$tests = @(
    (Make-TestPath "build-ci/shared/tests"),
    (Make-TestPath "build-ci/server/tests"),
    (Make-TestPath "build-ci/client/windows/tests")
)

foreach ($dir in $tests) {
    if (-not (Test-Path $dir)) {
        Write-Host "[tests] skip $dir (not found)"
        continue
    }
    Write-Host "[tests] running ctest in $dir"
    Push-Location $dir
    try {
        ctest --output-on-failure
    }
    finally {
        Pop-Location
    }
}

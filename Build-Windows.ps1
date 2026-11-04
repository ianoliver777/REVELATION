[CmdletBinding()]
param([string]$JuceDir = "", [switch]$BuildOnly)

$ErrorActionPreference = "Stop"
$source = $PSScriptRoot
$build = Join-Path $source "build-windows-x64"

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program exited with code $LASTEXITCODE"
    }
}

$configure = @("-S", $source, "-B", $build, "-G", "Visual Studio 17 2022", "-A", "x64", "-DREVELATION_TESTS=ON")
if ($JuceDir) {
    $configure += "-DJUCE_DIR=$((Resolve-Path -LiteralPath $JuceDir).Path)"
}
Invoke-Checked cmake $configure
Invoke-Checked cmake @("--build", $build, "--config", "Release", "--target", "Revelation_VST3", "DspTest", "HostTest", "--parallel")
Invoke-Checked ctest @("--test-dir", $build, "-C", "Release", "--output-on-failure")

$bundle = Join-Path $build "Revelation_artefacts\Release\VST3\REVELATION.vst3"
$hostTest = Join-Path $build "HostTest_artefacts\Release\HostTest.exe"
if (-not $BuildOnly) {
    Invoke-Checked $hostTest @($bundle)
}
Write-Host "VST3 ready at $bundle"

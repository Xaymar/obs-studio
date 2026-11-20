#Requires -Version 5.1
<#
.SYNOPSIS
Builds libobs and its unit tests with LLVM source-based coverage, runs ctest and writes coverage reports for obs.dll.

.EXAMPLE
pwsh test/coverage/run-coverage.ps1
#>
param(
    [string] $BuildDir = 'build_clangcov',
    [string] $Config = 'Debug',
    [string] $LlvmRoot = 'C:\Program Files\LLVM',
    [string] $Generator = 'Visual Studio 18 2026'
)

$ErrorActionPreference = 'Stop'
$SourceDir = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $SourceDir $BuildDir }
$LlvmBin = Join-Path $LlvmRoot 'bin'
$CovDir = Join-Path $BuildDir 'coverage'

function Invoke-Checked {
    param([string] $Exe, [string[]] $Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

Invoke-Checked cmake @('-S', $SourceDir, '-B', $BuildDir, '-G', $Generator, '-A', 'x64', '-T', 'ClangCL',
    "-DLLVM_ROOT=$($LlvmRoot -replace '\\', '/')", '-C', (Join-Path $PSScriptRoot 'clangcov.cmake'))
# Not ALL_BUILD: libobs-winrt does not compile with clang-cl (STL1009). obs-unit-tests depends on every test.
Invoke-Checked cmake @('--build', $BuildDir, '--config', $Config, '--parallel', '--target', 'libobs', 'obs-unit-tests')

# Fresh profiles for every run
if (Test-Path $CovDir) { Remove-Item -Recurse -Force $CovDir }
New-Item -ItemType Directory $CovDir | Out-Null

$depsBin = Get-ChildItem (Join-Path $SourceDir '.deps') -Directory -Filter 'obs-deps-*-x64' |
    ForEach-Object { Join-Path $_.FullName 'bin' }
$env:PATH = (@("$BuildDir\rundir\$Config\bin\64bit") + $depsBin + $env:PATH) -join ';'
# %p: one file per process, %m: one per instrumented module
$env:LLVM_PROFILE_FILE = Join-Path $CovDir '%p-%m.profraw'

& ctest --test-dir $BuildDir -C $Config --output-on-failure
$ctestExit = $LASTEXITCODE

$profiles = @(Get-ChildItem $CovDir -Filter *.profraw | ForEach-Object FullName)
if ($profiles.Count -eq 0) { throw 'No profiles were written; did any instrumented test run?' }
$profdata = Join-Path $CovDir 'merged.profdata'
Invoke-Checked (Join-Path $LlvmBin 'llvm-profdata.exe') (@('merge', '-sparse', '-o', $profdata) + $profiles)

# The test executables carry their own copies of libobs' header-inline helpers (darray.h, deque.h, ...)
$testObjects = @(Get-ChildItem "$BuildDir\test" -Recurse -Filter *.exe | Where-Object { $_.DirectoryName -like "*\$Config" } |
    ForEach-Object { "-object=$($_.FullName)" })
$covArgs = @("$BuildDir\libobs\$Config\obs.dll") + $testObjects + @("-instr-profile=$profdata",
    '-ignore-filename-regex=([\\/]\.deps[\\/]|[\\/]test[\\/]|[\\/]build[^\\/]*[\\/])')
$llvmCov = Join-Path $LlvmBin 'llvm-cov.exe'
& $llvmCov report @covArgs | Tee-Object (Join-Path $CovDir 'report.txt')
if ($LASTEXITCODE -ne 0) { throw "llvm-cov report failed with exit code $LASTEXITCODE" }
Invoke-Checked $llvmCov (@('show', '-format=html', "-output-dir=$CovDir\html") + $covArgs)

Write-Host "Coverage report: $CovDir\report.txt"
Write-Host "HTML report:     $CovDir\html\index.html"
exit $ctestExit

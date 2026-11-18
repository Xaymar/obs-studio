# Queue priority measurement runner (docs/superpowers/specs/2026-11-17-libobs-vulkan-priority-measurement.md).
# Not run by ctest. Runs the synthetic D3D11 foreground load alone and next to the OBS side load on each backend,
# repetitions interleaved (rep 1 of every scenario, then rep 2, ...), and writes per-run CSVs plus summary.csv.
#   powershell -ExecutionPolicy Bypass -File test\cmocka\vulkan_priority_bench.ps1 -Out <dir>
param(
	[string]$Build,
	[string]$Deps,
	[Parameter(Mandatory = $true)][string]$Out,
	[int]$Seconds = 30,
	[int]$Warmup = 5,
	[int]$Reps = 3,
	[int]$Iters = 1536
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path # PowerShell 5.1 has no $PSScriptRoot in param defaults
if (-not $Build) { $Build = "$root\build_x64" }
if (-not $Deps) { $Deps = "$root\.deps\obs-deps-2026-10-23-x64\bin" }
$Build = (Resolve-Path $Build).Path
$bin = "$Build\test\cmocka\RelWithDebInfo"
$env:PATH = "$Build\libobs\RelWithDebInfo;$Build\deps\w32-pthreads\RelWithDebInfo;$((Resolve-Path $Deps).Path);$env:PATH"
Remove-Item Env:OBS_VULKAN_VALIDATION, Env:OBS_VULKAN_REALTIME -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path

$vulkan = "$Build\libobs-vulkan\RelWithDebInfo\libobs-vulkan"
$d3d11 = "$Build\libobs-d3d11\RelWithDebInfo\libobs-d3d11"
# name, module, load kind, OBS_VULKAN_REALTIME
$scenarios = @(
	@('a_baseline', $null, $null, $null),
	@('b0_d3d11_as_built', $d3d11, 'd3d11', $null),
	@('b1_d3d11_release', $d3d11, 'd3d11-release', $null),
	@('c_vulkan_default', $vulkan, 'vulkan', $null),
	@('d_vulkan_realtime', $vulkan, 'vulkan', '1')
)
$summary = "$Out\summary.csv"
'scenario,rep,frames,mean_ms,p50_ms,p95_ms,p99_ms,max_ms,foreground,fg_cpu_s,gpu3d_fg,gpu3d_load,gpu3d_other,load' |
	Set-Content $summary

# 3D engine utilisation per process id: Windows "GPU Engine" counters, percent, averaged over the measured window.
$sampler = {
	param($wait, $n)
	Start-Sleep $wait
	$s = Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage' -SampleInterval 1 -MaxSamples $n
	$s.CounterSamples | Group-Object { [int]($_.InstanceName -replace '^pid_(\d+)_.*', '$1') } | ForEach-Object {
		[pscustomobject]@{ pid = [int]$_.Name; avg = ($_.Group | Measure-Object CookedValue -Sum).Sum / $n }
	}
}

for ($rep = 1; $rep -le $Reps; $rep++) {
	foreach ($s in $scenarios) {
		$name, $module, $kind, $rt = $s
		$tag = "${name}_rep$rep"
		$load = $null
		if ($module) {
			if ($rt) { $env:OBS_VULKAN_REALTIME = $rt }
			$load = Start-Process "$bin\test_vulkan_video.exe" -PassThru -NoNewWindow `
				-ArgumentList 'load', $module, $kind, ($Seconds + $Warmup + 6) `
				-RedirectStandardOutput "$Out\$tag.load.txt" -RedirectStandardError "$Out\$tag.load.err.txt"
			Remove-Item Env:OBS_VULKAN_REALTIME -ErrorAction SilentlyContinue
			for ($i = 0; $i -lt 100 -and -not (Select-String -Quiet 'LOAD RUNNING' "$Out\$tag.load.txt"); $i++) {
				Start-Sleep -Milliseconds 200
			}
			Start-Sleep 1
		}
		$fg = Start-Process "$bin\vulkan_priority_fg_load.exe" -PassThru -NoNewWindow `
			-ArgumentList $Seconds, $Warmup, $Iters, "$Out\$tag.csv" -RedirectStandardOutput "$Out\$tag.fg.txt"
		$job = Start-Job $sampler -ArgumentList ($Warmup + 1), ($Seconds - 2)
		$fg.WaitForExit()
		$util = Receive-Job $job -Wait -AutoRemoveJob
		$line = Get-Content "$Out\$tag.fg.txt" -Raw
		$loadline = ''
		if ($load) {
			$load.WaitForExit()
			$loadline = ((Select-String 'load .*frames|load cpu|granted|d3d11-release' "$Out\$tag.load.txt").Line -join ' | ')
		}
		$g_fg = ($util | Where-Object pid -eq $fg.Id | Measure-Object avg -Sum).Sum
		$g_load = 0
		if ($load) { $g_load = ($util | Where-Object pid -eq $load.Id | Measure-Object avg -Sum).Sum }
		$g_all = ($util | Measure-Object avg -Sum).Sum
		$g = '{0:F1},{1:F1},{2:F1}' -f $g_fg, $g_load, ($g_all - $g_fg - $g_load)
		$util | Sort-Object avg -Descending | Select-Object -First 8 | ForEach-Object {
			'{0,7:F2}% pid {1} {2}' -f $_.avg, $_.pid, (Get-Process -Id $_.pid -ErrorAction SilentlyContinue).ProcessName
		} | Set-Content "$Out\$tag.gpu.txt"
		if ($line -notmatch 'frames (\d+) mean ([\d.]+) p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+) max ([\d.]+) ms; foreground (\w+); cpu ([\d.]+)') {
			throw "unexpected output: $line"
		}
		Write-Host "$tag $($line.Trim()) gpu3d fg,load,other $g $loadline"
		"$name,$rep,$($Matches[1..8] -join ','),$g,`"$loadline`"" | Add-Content $summary
		Start-Sleep 3
	}
}

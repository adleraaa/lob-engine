# Windows helper: waits until the machine is idle, then runs lob_bench and
# records the conditions it ran under in <Out>/bench_conditions.json:
# total CPU load sampled before and during the run, AC vs battery power, and
# the active power plan. The benchmark numbers are only as good as these
# conditions, so they are committed next to them.
#
# Usage (PowerShell 7):
#   pwsh -NoProfile -File scripts/bench_when_idle.ps1 [-MaxLoad 15] [-TimeoutMin 120]
param(
    [string]$Bench = "build/lob_bench.exe",
    [int]$Ops = 2000000,
    [int]$Reps = 11,
    [int]$LatReps = 5,
    [int]$Cpu = 4,
    [double]$MaxLoad = 15,   # percent of all logical CPUs
    [int]$QuietSamples = 8,  # consecutive 2-second samples below MaxLoad
    [int]$TimeoutMin = 120,
    [string]$Out = "results"
)
$ErrorActionPreference = "Stop"
$counter = '\Processor(_Total)\% Processor Time'

function Get-Load {
    [math]::Round((Get-Counter $counter -SampleInterval 2 -MaxSamples 1).CounterSamples[0].CookedValue, 1)
}

$deadline = (Get-Date).AddMinutes($TimeoutMin)
$quiet = [System.Collections.Generic.List[double]]::new()
while ($quiet.Count -lt $QuietSamples) {
    if ((Get-Date) -gt $deadline) {
        Write-Error "machine did not become idle (< $MaxLoad% CPU) within $TimeoutMin minutes; not benchmarking"
    }
    $load = Get-Load
    if ($load -lt $MaxLoad) { $quiet.Add($load) } else { $quiet.Clear() }
}

# Sample load in the background for as long as the benchmark runs. The
# benchmark itself keeps one logical CPU busy, which is part of these numbers.
$sampler = Start-Job -ScriptBlock {
    param($c)
    while ($true) {
        [math]::Round((Get-Counter $c -SampleInterval 2 -MaxSamples 1).CounterSamples[0].CookedValue, 1)
    }
} -ArgumentList $counter

$started = Get-Date
& $Bench --ops $Ops --reps $Reps --lat-reps $LatReps --cpu $Cpu --out $Out
$exit = $LASTEXITCODE
$finished = Get-Date
Stop-Job $sampler
# Cast to plain numbers: job output carries PowerShell metadata properties
# that would otherwise end up in the JSON.
$during = @(Receive-Job $sampler | ForEach-Object { [double]$_ })
Remove-Job $sampler
if ($exit -ne 0) { Write-Error "lob_bench failed with exit code $exit" }

$battery = Get-CimInstance Win32_Battery | Select-Object -First 1
# powercfg prints a localized sentence in the console code page, so keep only
# the scheme GUID and name the three built-in Windows schemes.
$planGuid = ((powercfg /getactivescheme) -join " ") -replace '.*([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}).*', '$1'
$knownPlans = @{
    '381b4222-f694-41f0-9685-ff5bb260df2e' = 'Balanced'
    '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c' = 'High performance'
    'a1841308-3541-4fab-bc81-f71556f20b4a' = 'Power saver'
}
$plan = if ($knownPlans.ContainsKey($planGuid)) { "$($knownPlans[$planGuid]) ($planGuid)" } else { $planGuid }
$sorted = $during | Sort-Object
$conditions = [ordered]@{
    started               = $started.ToString("s")
    seconds               = [math]::Round(($finished - $started).TotalSeconds, 1)
    logical_cpus          = [Environment]::ProcessorCount
    idle_threshold_pct    = $MaxLoad
    load_before_run_pct   = $quiet
    load_during_run_pct   = $during
    load_during_run_median = if ($sorted.Count) { $sorted[[int]($sorted.Count / 2)] } else { $null }
    load_during_run_max   = if ($sorted.Count) { $sorted[-1] } else { $null }
    on_ac_power           = if ($battery) { $battery.BatteryStatus -eq 2 } else { $true }
    power_plan            = $plan.Trim()
}
$conditions | ConvertTo-Json | Set-Content -Path (Join-Path $Out "bench_conditions.json") -Encoding utf8NoBOM
Write-Output "done: load before $($quiet -join ','), during median $($conditions.load_during_run_median) max $($conditions.load_during_run_max)"

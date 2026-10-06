[CmdletBinding()]
param(
    [string]$ProcessName = 'acheron',
    [int]$ProcessId,

    [Parameter(Mandatory)]
    [ValidateSet('A', 'B', 'C', 'D', 'E', 'F')]
    [string]$Scenario,

    [ValidateRange(0, 600)]
    [int]$WarmupSeconds = 10,

    [ValidateRange(3, 600)]
    [int]$Samples = 30,

    [ValidateRange(0.25, 60)]
    [double]$IntervalSeconds = 1,

    [string]$OutputDirectory = 'artifacts\benchmarks'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scenarioNames = @{
    A = 'App open, not logged in'
    B = 'Logged in and idle'
    C = 'Guild/channel view, not in voice'
    D = 'Connected to a voice channel'
    E = 'Receiving voice audio'
    F = 'Transmitting microphone audio'
}

function Get-PrivateWorkingSet([int]$Id) {
    try {
        $counterSet = Get-Counter @(
            '\Process(*)\ID Process',
            '\Process(*)\Working Set - Private'
        ) -ErrorAction Stop

        $ids = @{}
        $privateWorkingSets = @{}
        foreach ($counter in $counterSet.CounterSamples) {
            if ($counter.Path -like '*\id process') {
                $ids[$counter.InstanceName] = [int]$counter.CookedValue
            } elseif ($counter.Path -like '*\working set - private') {
                $privateWorkingSets[$counter.InstanceName] = [double]$counter.CookedValue
            }
        }
        foreach ($instance in $ids.Keys) {
            if ($ids[$instance] -eq $Id -and $privateWorkingSets.ContainsKey($instance)) {
                return $privateWorkingSets[$instance]
            }
        }
    } catch {
        return $null
    }
    return $null
}

function Get-Stats([object[]]$Rows, [string]$Property, [double]$Scale = 1.0) {
    $values = @($Rows | ForEach-Object { $_.$Property } | Where-Object { $null -ne $_ })
    if ($values.Count -eq 0) {
        return [ordered]@{ Min = $null; Average = $null; Max = $null }
    }
    $measurement = $values | Measure-Object -Minimum -Average -Maximum
    return [ordered]@{
        Min = [math]::Round($measurement.Minimum / $Scale, 2)
        Average = [math]::Round($measurement.Average / $Scale, 2)
        Max = [math]::Round($measurement.Maximum / $Scale, 2)
    }
}

if ($ProcessId) {
    $target = Get-Process -Id $ProcessId -ErrorAction Stop
    $ProcessName = $target.ProcessName
} else {
    $target = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Sort-Object StartTime -Descending |
        Select-Object -First 1
    if (-not $target) {
        throw "No running process named '$ProcessName' was found. Start the app first."
    }
    $ProcessId = $target.Id
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputRoot = if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
    [System.IO.Path]::GetFullPath($OutputDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot $OutputDirectory))
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

Write-Host "Scenario ${Scenario}: $($scenarioNames[$Scenario])" -ForegroundColor Cyan
Write-Host "PID $ProcessId ($ProcessName); warm-up ${WarmupSeconds}s; $Samples samples every ${IntervalSeconds}s"
if ($WarmupSeconds -gt 0) {
    Start-Sleep -Seconds $WarmupSeconds
}

$target = Get-Process -Id $ProcessId -ErrorAction Stop
$target.Refresh()
$previousCpu = $target.TotalProcessorTime.TotalMilliseconds
$previousTime = [DateTime]::UtcNow
$logicalProcessors = [Environment]::ProcessorCount
$rows = [System.Collections.Generic.List[object]]::new()

for ($index = 1; $index -le $Samples; $index++) {
    Start-Sleep -Milliseconds ([math]::Round($IntervalSeconds * 1000))

    $target = Get-Process -Id $ProcessId -ErrorAction Stop
    $target.Refresh()
    $now = [DateTime]::UtcNow
    $cpuNow = $target.TotalProcessorTime.TotalMilliseconds
    $wallMilliseconds = ($now - $previousTime).TotalMilliseconds
    $cpuPercent = if ($wallMilliseconds -gt 0) {
        (($cpuNow - $previousCpu) / $wallMilliseconds) * 100.0 / $logicalProcessors
    } else {
        0.0
    }

    $privateWorkingSet = Get-PrivateWorkingSet $ProcessId
    $processCount = @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue).Count
    $rows.Add([pscustomobject][ordered]@{
        TimestampUtc = $now.ToString('o')
        Scenario = $Scenario
        Sample = $index
        ProcessId = $ProcessId
        ProcessCount = $processCount
        WorkingSetBytes = [int64]$target.WorkingSet64
        PrivateWorkingSetBytes = if ($null -eq $privateWorkingSet) { $null } else { [int64]$privateWorkingSet }
        PrivateBytes = [int64]$target.PrivateMemorySize64
        CpuPercentNormalized = [math]::Round([math]::Max(0, $cpuPercent), 3)
        Threads = $target.Threads.Count
        Handles = $target.HandleCount
        UptimeSeconds = [math]::Round(($now.ToLocalTime() - $target.StartTime).TotalSeconds, 2)
    })

    $previousCpu = $cpuNow
    $previousTime = $now
    Write-Progress -Activity "Benchmark scenario $Scenario" -Status "$index / $Samples" -PercentComplete (($index / $Samples) * 100)
}
Write-Progress -Activity "Benchmark scenario $Scenario" -Completed

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$baseName = "scenario-$Scenario-$stamp"
$samplesPath = Join-Path $outputRoot "$baseName-samples.csv"
$summaryPath = Join-Path $outputRoot "$baseName-summary.json"
$rows | Export-Csv -LiteralPath $samplesPath -NoTypeInformation -Encoding UTF8

$summary = [ordered]@{
    RecordedAt = (Get-Date).ToString('o')
    Scenario = $Scenario
    Description = $scenarioNames[$Scenario]
    ProcessName = $ProcessName
    ProcessId = $ProcessId
    LogicalProcessors = $logicalProcessors
    WarmupSeconds = $WarmupSeconds
    SampleCount = $Samples
    IntervalSeconds = $IntervalSeconds
    WorkingSetMiB = Get-Stats $rows 'WorkingSetBytes' 1MB
    PrivateWorkingSetMiB = Get-Stats $rows 'PrivateWorkingSetBytes' 1MB
    PrivateBytesMiB = Get-Stats $rows 'PrivateBytes' 1MB
    CpuPercentNormalized = Get-Stats $rows 'CpuPercentNormalized'
    ProcessCount = Get-Stats $rows 'ProcessCount'
    Threads = Get-Stats $rows 'Threads'
    Handles = Get-Stats $rows 'Handles'
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $summaryPath -Encoding UTF8

$display = @(
    [pscustomobject]@{ Metric = 'Working Set (MiB)'; Min = $summary.WorkingSetMiB.Min; Average = $summary.WorkingSetMiB.Average; Max = $summary.WorkingSetMiB.Max }
    [pscustomobject]@{ Metric = 'Private Working Set (MiB)'; Min = $summary.PrivateWorkingSetMiB.Min; Average = $summary.PrivateWorkingSetMiB.Average; Max = $summary.PrivateWorkingSetMiB.Max }
    [pscustomobject]@{ Metric = 'Private Bytes (MiB)'; Min = $summary.PrivateBytesMiB.Min; Average = $summary.PrivateBytesMiB.Average; Max = $summary.PrivateBytesMiB.Max }
    [pscustomobject]@{ Metric = 'CPU (% of machine)'; Min = $summary.CpuPercentNormalized.Min; Average = $summary.CpuPercentNormalized.Average; Max = $summary.CpuPercentNormalized.Max }
    [pscustomobject]@{ Metric = 'Processes'; Min = $summary.ProcessCount.Min; Average = $summary.ProcessCount.Average; Max = $summary.ProcessCount.Max }
    [pscustomobject]@{ Metric = 'Threads'; Min = $summary.Threads.Min; Average = $summary.Threads.Average; Max = $summary.Threads.Max }
    [pscustomobject]@{ Metric = 'Handles'; Min = $summary.Handles.Min; Average = $summary.Handles.Average; Max = $summary.Handles.Max }
)

$display | Format-Table -AutoSize
Write-Host "Samples: $samplesPath"
Write-Host "Summary: $summaryPath"

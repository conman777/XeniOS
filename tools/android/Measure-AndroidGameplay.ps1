# Collect an already-running, warmed gameplay scene without changing device data.
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9._:-]+$')]
    [string]$Serial,
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe",
    [ValidatePattern('^[A-Za-z0-9._]+$')]
    [string]$Package = 'jp.xenios.emulator.github.debug',
    [string]$OutputRoot = (Join-Path (Get-Location) 'scratch/android-perf'),
    [ValidateRange(15, 600)]
    [int]$Seconds = 120,
    [string]$LogPath,
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'

function Get-PerformanceSummary {
    param([string[]]$Lines, [int]$DiscardSamples = 1)
    $samples = @()
    $sample = $null
    $previousTotal = -1L
    foreach ($line in $Lines) {
        if ($line -match 'SwapSummary dt_ms=(\d+) swaps=(\d+) \(\+(\d+)\) refreshed=\d+ \(\+(\d+)\)') {
            $total = [long]$Matches[2]
            if ($total -lt $previousTotal) { throw 'Guest frame counter reset: benchmark spans a restart.' }
            $previousTotal = $total
            $sample = [PSCustomObject]@{
                Milliseconds = [long]$Matches[1]
                Swaps = [long]$Matches[3]
                Refreshed = [long]$Matches[4]
                GpuMilliseconds = $null
                Compiling = $false
            }
            if ($line -match 'placeholders=(\d+).*queued=(\d+) busy=(\d+)') {
                $sample.Compiling = ([long]$Matches[1] + [long]$Matches[2] + [long]$Matches[3]) -gt 0
            }
            $samples += $sample
        } elseif ($null -ne $sample -and $line -match 'GpuTime per_swap total=([0-9.]+)ms') {
            $sample.GpuMilliseconds = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
        }
    }
    # The first interval may start before collection. Drop it, not the whole log buffer.
    $samples = @($samples | Select-Object -Skip $DiscardSamples)
    if (!$samples.Count) { throw 'No complete gameplay summaries. Check halo_android_diagnostics and log_level.' }
    $milliseconds = ($samples | Measure-Object Milliseconds -Sum).Sum
    if ($milliseconds -le 0) { throw 'No elapsed time in gameplay summaries.' }
    $gpuSamples = @($samples | Where-Object { $null -ne $_.GpuMilliseconds -and $_.Swaps -gt 0 })
    $gpuMilliseconds = $null
    if ($gpuSamples.Count) {
        $gpuFrames = ($gpuSamples | Measure-Object Swaps -Sum).Sum
        $gpuTime = ($gpuSamples | ForEach-Object { $_.GpuMilliseconds * $_.Swaps } | Measure-Object -Sum).Sum
        $gpuMilliseconds = [math]::Round($gpuTime / $gpuFrames, 2)
    }
    [PSCustomObject]@{
        Samples = $samples.Count
        MeasuredSeconds = [math]::Round($milliseconds / 1000, 2)
        SwapFps = [math]::Round(1000 * ($samples | Measure-Object Swaps -Sum).Sum / $milliseconds, 2)
        RefreshFps = [math]::Round(1000 * ($samples | Measure-Object Refreshed -Sum).Sum / $milliseconds, 2)
        GpuMillisecondsPerSwap = $gpuMilliseconds
        SamplesWithCompilation = @($samples | Where-Object Compiling).Count
    }
}

if ($SelfTest) {
    $fixture = @(
        'SwapSummary dt_ms=5000 swaps=50 (+50) refreshed=50 (+50)',
        'GpuTime per_swap total=20.0ms',
        'SwapSummary dt_ms=10000 swaps=250 (+200) refreshed=150 (+100)',
        'GpuTime per_swap total=40.0ms'
    )
    $summary = Get-PerformanceSummary $fixture 0
    if ($summary.SwapFps -ne 16.67 -or $summary.RefreshFps -ne 10 -or $summary.GpuMillisecondsPerSwap -ne 36) {
        throw 'Weighted timing / refreshed-frame accounting failed.'
    }
    if ((Get-PerformanceSummary $fixture).SwapFps -ne 20) { throw 'Partial first interval was not discarded.' }
    $restartRejected = $false
    try { Get-PerformanceSummary ($fixture + 'SwapSummary dt_ms=5000 swaps=10 (+10) refreshed=10 (+10)') 0 | Out-Null }
    catch { $restartRejected = $_.Exception.Message -like 'Guest frame counter reset:*' }
    if (!$restartRejected) { throw 'Restarted process was accepted as one benchmark.' }
    Write-Host 'PASS: duration-weighted FPS, refreshed frames, GPU weighting, partial intervals and restart rejection.'
    return
}
if ($LogPath) {
    Get-PerformanceSummary (Get-Content -LiteralPath $LogPath) | ConvertTo-Json
    return
}
if (!$Serial) { throw 'Specify -Serial for the device to measure.' }
if (!(Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw "ADB not found: $AdbPath" }
function Invoke-Adb {
    param([string[]]$Arguments)
    $result = & $AdbPath -s $Serial @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "ADB failed: $($Arguments -join ' ')`n$($result -join "`n")" }
    ($result -join "`n").Trim()
}
function Save-Text {
    param([string]$Name, [string]$Value)
    [IO.File]::WriteAllText((Join-Path $outputDirectory $Name), $Value + "`n", [Text.UTF8Encoding]::new($false))
}
if ((Invoke-Adb @('get-state')) -ne 'device') { throw 'ADB device is not ready.' }
$processIdBefore = Invoke-Adb @('shell', "pidof $Package")
if (!$processIdBefore) { throw 'Start Halo and warm the chosen gameplay scene before measuring.' }
$outputDirectory = Join-Path $OutputRoot ([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $outputDirectory | Out-Null
$externalRoot = "/sdcard/Android/data/$Package/files"
Save-Text 'internal-profile.txt' (Invoke-Adb @('shell', "run-as $Package cat files/xenios_android_profile.txt"))
Save-Text 'saved-config.toml' (Invoke-Adb @('shell', "run-as $Package cat files/xenios.config.toml"))
foreach ($file in @('xenios_android_profile.txt', 'halo_experiment.txt')) {
    Save-Text $file (Invoke-Adb @('shell', "if [ -f '$externalRoot/$file' ]; then cat '$externalRoot/$file'; fi"))
}
Save-Text 'startup-log.txt' (Invoke-Adb @('logcat', '-d', '-v', 'threadtime', '-s', 'xenia:*'))
$apkPaths = (Invoke-Adb @('shell', "pm path $Package")) -split '\r?\n'
$apkHashes = @()
for ($i = 0; $i -lt $apkPaths.Count; ++$i) {
    $apkFile = Join-Path $outputDirectory "installed-$i.apk"
    Invoke-Adb @('pull', ($apkPaths[$i] -replace '^package:', ''), $apkFile) | Out-Null
    $apkHashes += (Get-FileHash -LiteralPath $apkFile -Algorithm SHA256).Hash
}
$metadata = [ordered]@{ Serial = $Serial; Package = $Package; PidBefore = $processIdBefore; ApkSha256 = $apkHashes; StartedUtc = [DateTime]::UtcNow.ToString('o'); RequestedSeconds = $Seconds }
Save-Text 'metadata.json' ($metadata | ConvertTo-Json)
$logFile = Join-Path $outputDirectory 'logcat.txt'
$logProcess = Start-Process -FilePath $AdbPath -WindowStyle Hidden -PassThru `
    -ArgumentList @('-s', $Serial, 'logcat', '-v', 'threadtime', '-T', '1', 'xenia:V', '*:W') `
    -RedirectStandardOutput $logFile -RedirectStandardError (Join-Path $outputDirectory 'logcat-stderr.txt')
$gpuSamples = @()
try {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
        Start-Sleep -Seconds 5
        if ($logProcess.HasExited) { throw 'Logcat collector stopped early.' }
        $gpuSamples += [PSCustomObject]@{
            Seconds = [math]::Round($timer.Elapsed.TotalSeconds, 1)
            Readings = Invoke-Adb @('shell', 'for f in gpu_busy_percentage gpuclk; do echo $f; cat /sys/class/kgsl/kgsl-3d0/$f 2>/dev/null || true; done')
        }
        Write-Host ("Collecting gameplay: {0}/{1}s" -f [int]$timer.Elapsed.TotalSeconds, $Seconds)
    }
} finally {
    if (!$logProcess.HasExited) { Stop-Process -Id $logProcess.Id }
}
$metadata.PidAfter = Invoke-Adb @('shell', "pidof $Package")
Save-Text 'metadata.json' ($metadata | ConvertTo-Json)
Save-Text 'gpu-samples.json' ($gpuSamples | ConvertTo-Json)
Save-Text 'meminfo.txt' (Invoke-Adb @('shell', "dumpsys meminfo $Package"))
$screenshot = Start-Process -FilePath $AdbPath -WindowStyle Hidden -PassThru -Wait `
    -ArgumentList @('-s', $Serial, 'exec-out', 'screencap', '-p') `
    -RedirectStandardOutput (Join-Path $outputDirectory 'screen.png') `
    -RedirectStandardError (Join-Path $outputDirectory 'screen-stderr.txt')
if ($screenshot.ExitCode -ne 0) { throw 'Screenshot capture failed.' }
if ($metadata.PidAfter -ne $metadata.PidBefore) { throw 'App process changed during the benchmark.' }
$summary = Get-PerformanceSummary (Get-Content -LiteralPath $logFile)
Save-Text 'summary.json' ($summary | ConvertTo-Json)
$summary | Format-List
Write-Host "Benchmark saved to $outputDirectory"

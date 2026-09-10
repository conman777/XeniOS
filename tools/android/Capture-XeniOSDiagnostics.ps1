[CmdletBinding()]
param(
    [string]$Serial,
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe",
    [string]$Package = "jp.xenios.emulator.github.debug",
    [string]$OutputRoot = (Join-Path (Get-Location) "xenios-diagnostics"),
    [ValidateRange(1, 600)]
    [int]$WaitSeconds = 120,
    [switch]$RenderDocTargetVerified,
    [string]$RenderDocTriggerScript,
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"

function Test-RequestId {
    param([string]$RequestId)
    return $RequestId -match '^[0-9]{8}T[0-9]{6}\.[0-9]{3}Z-p[0-9]+-e[0-9]+$'
}

function Assert-RenderDocOptions {
    param(
        [bool]$TargetVerified,
        [string]$TriggerScript
    )
    if ($TargetVerified -and [string]::IsNullOrWhiteSpace($TriggerScript)) {
        throw "RenderDocTargetVerified requires RenderDocTriggerScript."
    }
    if (-not $TargetVerified -and -not [string]::IsNullOrWhiteSpace($TriggerScript)) {
        throw "RenderDocTriggerScript is refused until RenderDocTargetVerified is set."
    }
}

function Invoke-SelfTest {
    if (-not (Test-RequestId "20260725T123456.789Z-p123-e456")) {
        throw "valid request ID was rejected"
    }
    if (Test-RequestId "../request") {
        throw "unsafe request ID was accepted"
    }
    try {
        Assert-RenderDocOptions -TargetVerified $false -TriggerScript "trigger.ps1"
        throw "unverified RenderDoc trigger was accepted"
    } catch {
        if ($_.Exception.Message -eq "unverified RenderDoc trigger was accepted") {
            throw
        }
    }
    Assert-RenderDocOptions -TargetVerified $false -TriggerScript ""
    Write-Host "Capture-XeniOSDiagnostics self-test passed."
}

if ($SelfTest) {
    Invoke-SelfTest
    return
}

Assert-RenderDocOptions `
    -TargetVerified $RenderDocTargetVerified.IsPresent `
    -TriggerScript $RenderDocTriggerScript

if (-not (Test-Path -LiteralPath $AdbPath -PathType Leaf)) {
    throw "ADB was not found at '$AdbPath'."
}
if ([string]::IsNullOrWhiteSpace($Serial)) {
    throw "Serial is required when more than one ADB target may be present."
}
if ($RenderDocTargetVerified -and
    -not (Test-Path -LiteralPath $RenderDocTriggerScript -PathType Leaf)) {
    throw "RenderDoc trigger script was not found at '$RenderDocTriggerScript'."
}

$remoteDiagnosticsRoot = "/sdcard/Android/data/$Package/files/diagnostics"
$adbPrefix = @("-s", $Serial)

function Invoke-AdbText {
    param([string[]]$Arguments)
    $output = & $AdbPath @adbPrefix @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "ADB failed ($LASTEXITCODE): $($Arguments -join ' ')`n$($output -join "`n")"
    }
    return ($output -join "`n").Trim()
}

function Save-AdbText {
    param(
        [string[]]$Arguments,
        [string]$Destination
    )
    $output = & $AdbPath @adbPrefix @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    [System.IO.File]::WriteAllText(
        $Destination,
        (($output -join "`r`n") + "`r`n"),
        [System.Text.UTF8Encoding]::new($false))
    if ($exitCode -ne 0) {
        throw "ADB failed ($exitCode): $($Arguments -join ' ')"
    }
}

function Save-Screenshot {
    param([string]$Destination)
    $stderrPath = "$Destination.stderr.txt"
    $arguments = @("-s", $Serial, "exec-out", "screencap", "-p")
    $process = Start-Process `
        -FilePath $AdbPath `
        -ArgumentList $arguments `
        -NoNewWindow `
        -Wait `
        -PassThru `
        -RedirectStandardOutput $Destination `
        -RedirectStandardError $stderrPath
    if ($process.ExitCode -ne 0) {
        throw "ADB screencap failed with exit code $($process.ExitCode)."
    }
    if ((Get-Item -LiteralPath $Destination).Length -eq 0) {
        throw "ADB screencap returned an empty file."
    }
}

Write-Host "Waiting for a DEBUG diagnostic request from $Package on $Serial..."
$deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
$requestId = $null
while ([DateTime]::UtcNow -lt $deadline) {
    $candidate = Invoke-AdbText @(
        "shell",
        "if [ -f '$remoteDiagnosticsRoot/LATEST_REQUEST' ]; then " +
        "cat '$remoteDiagnosticsRoot/LATEST_REQUEST'; fi"
    )
    if (Test-RequestId $candidate) {
        $ready = Invoke-AdbText @(
            "shell",
            "if [ -f '$remoteDiagnosticsRoot/requests/$candidate/READY' ]; then echo ready; fi"
        )
        if ($ready -eq "ready") {
            $requestId = $candidate
            break
        }
    }
    Start-Sleep -Milliseconds 500
}
if ($null -eq $requestId) {
    throw "No complete diagnostic request appeared within $WaitSeconds seconds."
}

$bundleDirectory = Join-Path $OutputRoot $requestId
if (Test-Path -LiteralPath $bundleDirectory) {
    throw "Bundle already exists: '$bundleDirectory'."
}
New-Item -ItemType Directory -Path $bundleDirectory -Force | Out-Null

$remoteRequest = "$remoteDiagnosticsRoot/requests/$requestId"
$appRequestDirectory = Join-Path $bundleDirectory "app-request"
Invoke-AdbText @("pull", $remoteRequest, $appRequestDirectory) | Out-Null

# Keep the screenshot close to the user marker before slower dumpsys commands.
Save-Screenshot (Join-Path $bundleDirectory "android-screen.png")

# Bounded to the newest 4,000 lines. The in-app BEGIN/READY markers correlate
# this window with request.json wall-clock and elapsed-realtime timestamps.
Save-AdbText `
    @("logcat", "-d", "-v", "epoch", "-t", "4000") `
    (Join-Path $bundleDirectory "logcat-bounded.txt")
Save-AdbText `
    @("shell", "dumpsys meminfo $Package") `
    (Join-Path $bundleDirectory "app-meminfo.txt")
Save-AdbText `
    @("shell", "dumpsys gfxinfo $Package") `
    (Join-Path $bundleDirectory "app-gfxinfo.txt")
Save-AdbText `
    @("shell", "dumpsys package $Package") `
    (Join-Path $bundleDirectory "package-state.txt")
Save-AdbText `
    @("shell", "dumpsys activity activities") `
    (Join-Path $bundleDirectory "activity-state.txt")
Save-AdbText `
    @("shell", "getprop") `
    (Join-Path $bundleDirectory "device-properties.txt")
Save-AdbText `
    @("shell", "cat /proc/meminfo") `
    (Join-Path $bundleDirectory "device-meminfo.txt")

$pid = Invoke-AdbText @("shell", "pidof $Package")
if ($pid -match '^[0-9]+$') {
    Save-AdbText `
        @("shell", "cat /proc/$pid/status") `
        (Join-Path $bundleDirectory "process-status.txt")
    Save-AdbText `
        @("shell", "cat /proc/$pid/limits") `
        (Join-Path $bundleDirectory "process-limits.txt")
    Save-AdbText `
        @("shell", "ps -T -p $pid") `
        (Join-Path $bundleDirectory "process-threads.txt")
}

$renderDocStatusPath = Join-Path $bundleDirectory "renderdoc-status.txt"
if ($RenderDocTargetVerified) {
    $triggerOutput = & $RenderDocTriggerScript `
        -Serial $Serial `
        -Package $Package `
        -RequestId $requestId `
        -OutputDirectory $bundleDirectory 2>&1
    $triggerExitCode = $LASTEXITCODE
    [System.IO.File]::WriteAllText(
        $renderDocStatusPath,
        "Target independently verified as Vulkan by operator: yes`r`n" +
        "Single trigger invoked: yes`r`n" +
        "Exit code: $triggerExitCode`r`n" +
        ($triggerOutput -join "`r`n") + "`r`n",
        [System.Text.UTF8Encoding]::new($false))
    if ($triggerExitCode -ne 0) {
        throw "The single RenderDoc trigger failed with exit code $triggerExitCode."
    }
} else {
    [System.IO.File]::WriteAllText(
        $renderDocStatusPath,
        "Target independently verified as Vulkan by operator: no`r`n" +
        "Single trigger invoked: no`r`n" +
        "Reason: default-safe behavior; the app button cannot select or verify " +
        "the Vulkan producer in QRenderDoc.`r`n",
        [System.Text.UTF8Encoding]::new($false))
}

$collectedAt = [DateTime]::UtcNow.ToString("o")
Invoke-AdbText @(
    "shell",
    "printf '%s\n' '$collectedAt' > '$remoteRequest/HOST_COLLECTED'"
) | Out-Null

$manifest = [ordered]@{
    protocol_version = 1
    request_id = $requestId
    package_name = $Package
    serial = $Serial
    collected_at_utc = $collectedAt
    logcat_max_lines = 4000
    renderdoc_target_verified = $RenderDocTargetVerified.IsPresent
    renderdoc_trigger_invoked = $RenderDocTargetVerified.IsPresent
    creates_or_modifies_guest_save = $false
}
$manifest | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $bundleDirectory "host-manifest.json") -Encoding UTF8

Write-Host "Diagnostic bundle complete: $bundleDirectory"

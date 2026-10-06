#requires -Version 5.1
[CmdletBinding()]
param([string]$Commit)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repository = 'Dduy07037/discordCard'
$branch = 'feat/voice-golive'
$artifactName = 'acheron-windows-RelWithDebInfo'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $Program $($Arguments -join ' ')" }
}

function Install-PortableGh {
    # Use the official portable release when gh is missing. No admin rights or
    # package manager are required; verify the published checksum before use.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $architecture = 'amd64'
    if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64') {
        $architecture = 'arm64'
    }
    Write-Host "Downloading official portable GitHub CLI ($architecture)..."
    $headers = @{ 'User-Agent' = 'discordCard-bootstrap'; 'Accept' = 'application/vnd.github+json' }
    $release = Invoke-RestMethod -Uri 'https://api.github.com/repos/cli/cli/releases/latest' -Headers $headers -TimeoutSec 60
    $archives = @($release.assets | Where-Object { $_.name -match ('^gh_[0-9.]+_windows_' + $architecture + '\.zip$') })
    $checksumAssets = @($release.assets | Where-Object { $_.name -match '^gh_[0-9.]+_checksums\.txt$' })
    if ($archives.Count -ne 1 -or $checksumAssets.Count -ne 1) {
        throw 'Cannot find the official GitHub CLI ZIP/checksums. Install GitHub CLI from https://cli.github.com/ and retry.'
    }
    $archiveAsset = $archives[0]
    $checksumAsset = $checksumAssets[0]
    foreach ($asset in @($archiveAsset, $checksumAsset)) {
        if ($asset.browser_download_url -notmatch '^https://github\.com/cli/cli/releases/download/') {
            throw 'Unexpected GitHub CLI release download URL.'
        }
    }
    $toolsDirectory = Join-Path $env:LOCALAPPDATA ('discordCard\tools\gh-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $toolsDirectory -Force | Out-Null
    $archivePath = Join-Path $toolsDirectory 'gh.zip'
    $checksumPath = Join-Path $toolsDirectory 'checksums.txt'
    Invoke-WebRequest -UseBasicParsing -Uri $archiveAsset.browser_download_url -OutFile $archivePath -TimeoutSec 60
    Invoke-WebRequest -UseBasicParsing -Uri $checksumAsset.browser_download_url -OutFile $checksumPath -TimeoutSec 60
    $checksumText = Get-Content -LiteralPath $checksumPath -Raw
    $checksumPattern = '(?im)^([a-f0-9]{64})\s+\*?' + [Regex]::Escape($archiveAsset.name) + '\s*$'
    $checksumMatch = [Regex]::Match($checksumText, $checksumPattern)
    if (-not $checksumMatch.Success) { throw 'GitHub CLI archive is missing from the published checksums.' }
    $archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
    if ($archiveHash -ne $checksumMatch.Groups[1].Value) { throw 'GitHub CLI checksum mismatch. Nothing has been executed.' }
    $extractDirectory = Join-Path $toolsDirectory 'gh'
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractDirectory
    $executables = @(Get-ChildItem -LiteralPath $extractDirectory -Filter gh.exe -File -Recurse)
    if ($executables.Count -ne 1) { throw 'The official GitHub CLI archive does not contain exactly one gh.exe.' }
    $ghPath = $executables[0].FullName
    Invoke-Checked -Program $ghPath -Arguments @('--version') | Out-Host
    return $ghPath
}


function Find-Gh {
    $command = Get-Command gh -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    foreach ($path in @((Join-Path $env:ProgramFiles 'GitHub CLI\gh.exe'), (Join-Path $env:LOCALAPPDATA 'Programs\GitHub CLI\gh.exe'))) {
        if (Test-Path -LiteralPath $path) { return $path }
    }
    $cachedTools = Join-Path $env:LOCALAPPDATA 'discordCard\tools'
    if (Test-Path -LiteralPath $cachedTools) {
        $cached = @(Get-ChildItem -LiteralPath $cachedTools -Filter gh.exe -File -Recurse | Sort-Object LastWriteTime -Descending)
        if ($cached.Count -gt 0) { return $cached[0].FullName }
    }
    return (Install-PortableGh)
}

function Read-GhJson {
    param([string[]]$Arguments)
    $json = & $script:gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw "GitHub CLI failed: $($Arguments -join ' ')" }
    return (($json -join "`n") | ConvertFrom-Json)
}

try {
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or -not [Environment]::Is64BitOperatingSystem) {
        throw 'Use this script on 64-bit Windows.'
    }
    $script:gh = Find-Gh
    & $gh auth status --hostname github.com
    if ($LASTEXITCODE -ne 0) {
        Invoke-Checked -Program $gh -Arguments @('auth', 'login', '--hostname', 'github.com', '--git-protocol', 'https', '--web')
    }
    if ([string]::IsNullOrWhiteSpace($Commit)) {
        $head = Read-GhJson -Arguments @('api', "repos/$repository/commits/$branch")
        $Commit = $head.sha
    }
    if ($Commit -notmatch '^[a-f0-9]{40}$') { throw 'Commit must be a full 40-character Git SHA.' }
    Write-Host "Requested commit: $Commit"
    $runId = $null
    $deadline = (Get-Date).AddMinutes(5)
    do {
        $runs = @(Read-GhJson -Arguments @('run', 'list', '--repo', $repository, '--workflow', 'build.yml', '--branch', $branch,
            '--commit', $Commit, '--limit', '1', '--json', 'databaseId'))
        if ($runs.Count -gt 0) { $runId = $runs[0].databaseId; break }
        Start-Sleep -Seconds 5
    } while ((Get-Date) -lt $deadline)
    if (-not $runId) { throw "No Build workflow found for $Commit. Check https://github.com/$repository/actions." }
    $runUrl = "https://github.com/$repository/actions/runs/$runId"
    Write-Host "Build: $runUrl"
    Write-Host 'Waiting for the full Windows job. Other platform jobs have separate results.'
    $deadline = (Get-Date).AddMinutes(60)
    $lastStatus = ''
    $windowsReady = $false
    do {
        $response = Read-GhJson -Arguments @('api', "repos/$repository/actions/runs/$runId/jobs?per_page=100")
        $jobs = @($response.jobs | Where-Object { $_.name -eq 'build-windows (RelWithDebInfo)' })
        if ($jobs.Count -gt 1) { throw "Ambiguous Windows jobs. Check $runUrl." }
        if ($jobs.Count -eq 1) {
            $job = $jobs[0]
            $status = "$($job.status) / $($job.conclusion)"
            if ($status -ne $lastStatus) { Write-Host "Windows: $status"; $lastStatus = $status }
            if ($job.status -eq 'completed') {
                if ($job.conclusion -ne 'success') { throw "Windows build failed ($($job.conclusion)). Check $runUrl." }
                $windowsReady = $true
                break
            }
        }
        Start-Sleep -Seconds 15
    } while ((Get-Date) -lt $deadline)
    if (-not $windowsReady) { throw "Timed out waiting for Windows. Check $runUrl, then run this script again." }
    $artifacts = Read-GhJson -Arguments @('api', "repos/$repository/actions/runs/$runId/artifacts")
    $matching = @($artifacts.artifacts | Where-Object { $_.name -eq $artifactName -and -not $_.expired })
    if ($matching.Count -ne 1) { throw "Windows artifact is unavailable or expired. Check $runUrl." }
    $output = Join-Path $env:LOCALAPPDATA ('discordCard\windows-' + $Commit.Substring(0, 12) + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 6))
    Invoke-Checked -Program $gh -Arguments @('run', 'download', "$runId", '--repo', $repository, '--name', $artifactName, '--dir', $output)
    $exe = Join-Path $output 'acheron.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "acheron.exe is missing in $output." }
    $env:ACHERON_GOLIVE_PROBE = '0'
    Write-Host "Launching new build: $exe"
    Write-Host 'Ctrl+Shift+V > Watch streams / Share screen. Select quality and 30 or 60 FPS before Share.'
    Write-Host '1080p Maximum uses 12 Mbps at 30 FPS or 16 Mbps at 60 FPS. Viewing has zoom, Fit and Fullscreen controls.'
    Write-Host 'Viewing supports VP8/H264. The statistics row shows actual FPS and receive/decryption progress.'
    Start-Process -FilePath $exe -WorkingDirectory $output | Out-Null
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

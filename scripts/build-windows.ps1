[CmdletBinding()]
param(
    [ValidateSet('MinSizeRel', 'RelWithDebInfo', 'Release', 'Debug')]
    [string]$Configuration = 'MinSizeRel',

    [string]$BuildDirectory = 'build',

    [string]$QtRoot = $env:QT_ROOT_DIR,

    [switch]$SkipDependencyDownload,
    [switch]$SkipTests,
    [switch]$SkipDeploy
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = if ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
}

$qtVersion = '6.10.2'
$qtArch = 'win64_msvc2022_64'
$qtFolder = 'msvc2022_64'
$curlVersion = 'v2.0.0'
$ffmpegRelease = 'autobuild-2026-07-31-14-10'
$ffmpegBuild = 'ffmpeg-n8.1.2-34-g9b6c8969e0-win64-lgpl-shared-8.1'
$ffmpegSha256 = 'c222a490dde4e7059f45495deef6bfb98dbcacc2b43df5b607546252037aa95c'

$toolsRoot = Join-Path $repoRoot '.tools'
$depsRoot = Join-Path $repoRoot '.deps'
$curlRoot = Join-Path $depsRoot 'curl-impersonate'
$ffmpegRoot = Join-Path $depsRoot 'ffmpeg'
$vcpkgRoot = Join-Path $repoRoot 'vendor\vcpkg'

function Assert-Windows {
    if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) {
        throw 'This script builds the Windows x64 target and must run on Windows.'
    }
}

function Get-RequiredCommand([string]$Name) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "Required command '$Name' was not found in PATH."
    }
    return $command.Source
}

function Get-CanonicalPathEnvironment {
    $segments = [System.Collections.Generic.List[string]]::new()
    foreach ($entry in [System.Environment]::GetEnvironmentVariables().GetEnumerator()) {
        if ([string]$entry.Key -ieq 'Path') {
            foreach ($segment in ([string]$entry.Value -split ';')) {
                if ($segment -and -not $segments.Contains($segment)) {
                    $segments.Add($segment)
                }
            }
        }
    }
    return ($segments -join ';')
}

function Invoke-CleanProcess {
    param(
        [Parameter(Mandatory)] [string]$FilePath,
        [string[]]$Arguments = @(),
        [hashtable]$Environment = @{}
    )

    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $FilePath
    $start.WorkingDirectory = $repoRoot
    $start.UseShellExecute = $false
    $start.Environment.Clear()

    # EnvironmentVariable is case-insensitive on Windows, but some hosts expose
    # both Path and PATH. MSBuild's .NET task host rejects that duplicate pair.
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in [System.Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $key = [string]$entry.Key
        if ($key -ieq 'Path') {
            continue
        }
        if ($seen.Add($key)) {
            $start.Environment[$key] = [string]$entry.Value
        }
    }
    $start.Environment['Path'] = Get-CanonicalPathEnvironment

    foreach ($key in $Environment.Keys) {
        $start.Environment[[string]$key] = [string]$Environment[$key]
    }
    foreach ($argument in $Arguments) {
        [void]$start.ArgumentList.Add($argument)
    }

    Write-Host "> $FilePath $($Arguments -join ' ')" -ForegroundColor DarkGray
    $process = [System.Diagnostics.Process]::Start($start)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) {
        throw "Command failed with exit code $($process.ExitCode): $FilePath"
    }
}

function Install-QtIfNeeded {
    if (-not $QtRoot) {
        $script:QtRoot = Join-Path $toolsRoot "Qt\$qtVersion\$qtFolder"
    }
    $script:QtRoot = [System.IO.Path]::GetFullPath($QtRoot)

    if (Test-Path (Join-Path $QtRoot 'bin\qmake.exe')) {
        return
    }
    if ($SkipDependencyDownload) {
        throw "Qt $qtVersion was not found at '$QtRoot'. Set QT_ROOT_DIR/QtRoot or allow dependency download."
    }

    $python = Get-RequiredCommand 'python'
    $aqtRoot = Join-Path $toolsRoot 'aqt'
    $aqtExe = Join-Path $aqtRoot 'bin\aqt.exe'
    New-Item -ItemType Directory -Path $toolsRoot -Force | Out-Null
    if (-not (Test-Path $aqtExe)) {
        Invoke-CleanProcess $python @('-m', 'pip', 'install', '--disable-pip-version-check', '--target', $aqtRoot, 'aqtinstall==3.3.0')
    }
    Invoke-CleanProcess $aqtExe @(
        'install-qt', 'windows', 'desktop', $qtVersion, $qtArch,
        '--outputdir', (Join-Path $toolsRoot 'Qt'), '-m', 'qtimageformats'
    )

    if (-not (Test-Path (Join-Path $QtRoot 'bin\qmake.exe'))) {
        throw "Qt installation completed but qmake.exe is missing under '$QtRoot'."
    }
}

function Install-CurlIfNeeded {
    $curlLib = Join-Path $curlRoot 'lib\libcurl-impersonate_imp.lib'
    if (Test-Path $curlLib) {
        return
    }
    if ($SkipDependencyDownload) {
        throw "curl-impersonate was not found at '$curlRoot'."
    }

    $tar = Get-RequiredCommand 'tar'
    $downloadRoot = Join-Path $depsRoot 'downloads'
    $archive = Join-Path $downloadRoot "libcurl-impersonate-$curlVersion.x86_64-win32.tar.gz"
    New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null
    if (-not (Test-Path $archive)) {
        $url = "https://github.com/lexiforest/curl-impersonate/releases/download/$curlVersion/libcurl-impersonate-$curlVersion.x86_64-win32.tar.gz"
        Invoke-WebRequest -Uri $url -OutFile $archive
    }
    if (Test-Path $curlRoot) {
        Remove-Item -LiteralPath $curlRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $curlRoot -Force | Out-Null
    Invoke-CleanProcess $tar @('-xzf', $archive, '-C', $curlRoot)

    if (-not (Test-Path $curlLib)) {
        throw 'curl-impersonate extraction did not produce the expected import library.'
    }
}

function Install-FfmpegIfNeeded {
    if (Test-Path (Join-Path $ffmpegRoot 'lib\avcodec.lib')) {
        return
    }
    if ($SkipDependencyDownload) {
        throw "FFmpeg was not found at '$ffmpegRoot'."
    }

    $downloadRoot = Join-Path $depsRoot 'downloads'
    $archive = Join-Path $downloadRoot "$ffmpegBuild.zip"
    $extractRoot = Join-Path $depsRoot 'ffmpeg-extract'
    New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null
    if (-not (Test-Path $archive)) {
        $url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/$ffmpegRelease/$ffmpegBuild.zip"
        Invoke-WebRequest -Uri $url -OutFile $archive
    }

    $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $ffmpegSha256) {
        throw "FFmpeg archive hash mismatch: expected $ffmpegSha256, got $actualHash"
    }

    if (Test-Path $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    if (Test-Path $ffmpegRoot) {
        Remove-Item -LiteralPath $ffmpegRoot -Recurse -Force
    }
    Expand-Archive -LiteralPath $archive -DestinationPath $extractRoot -Force
    $inner = Get-ChildItem -LiteralPath $extractRoot -Directory | Select-Object -First 1
    if (-not $inner) {
        throw 'FFmpeg archive did not contain a top-level directory.'
    }
    Move-Item -LiteralPath $inner.FullName -Destination $ffmpegRoot
    Remove-Item -LiteralPath $extractRoot -Recurse -Force
}

Assert-Windows
$cmake = Get-RequiredCommand 'cmake'
$ctest = Get-RequiredCommand 'ctest'
$git = Get-RequiredCommand 'git'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found. Install Visual Studio 2022 with Desktop development with C++.'
}
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) {
    throw 'Visual Studio 2022 C++ x64 build tools were not found.'
}

Write-Host 'Synchronizing Git submodules...'
Invoke-CleanProcess $git @('submodule', 'sync', '--recursive')
Invoke-CleanProcess $git @('submodule', 'update', '--init', '--recursive')

$vcpkgExe = Join-Path $vcpkgRoot 'vcpkg.exe'
if (-not (Test-Path $vcpkgExe)) {
    $bootstrap = Join-Path $vcpkgRoot 'bootstrap-vcpkg.bat'
    if (-not (Test-Path $bootstrap)) {
        throw 'The vcpkg submodule is incomplete.'
    }
    Invoke-CleanProcess $env:ComSpec @('/d', '/c', $bootstrap)
}

Install-QtIfNeeded
Install-CurlIfNeeded
Install-FfmpegIfNeeded

$qtRootResolved = (Resolve-Path -LiteralPath $QtRoot).Path
$curlRootResolved = (Resolve-Path -LiteralPath $curlRoot).Path
$ffmpegRootResolved = (Resolve-Path -LiteralPath $ffmpegRoot).Path
$runtimePath = "$qtRootResolved\bin;$(Get-CanonicalPathEnvironment)"
$cleanEnvironment = @{ Path = $runtimePath; QT_ROOT_DIR = $qtRootResolved }

$configureArguments = @(
    '-S', $repoRoot,
    '-B', $buildRoot,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-DCMAKE_BUILD_TYPE=$Configuration",
    '-DVCPKG_TARGET_TRIPLET=x64-windows-static-md',
    "-DCMAKE_PREFIX_PATH=$qtRootResolved;$ffmpegRootResolved",
    "-DCURL_INCLUDE_DIR=$curlRootResolved\include",
    "-DCURL_LIBRARY=$curlRootResolved\lib\libcurl-impersonate_imp.lib",
    '-DBUILD_TESTS=ON',
    '-DENABLE_VOICE=ON',
    '-DENABLE_RNNOISE=ON',
    '-DENABLE_FFMPEG=ON'
)

Write-Host "Configuring Acheron ($Configuration, x64, voice enabled)..." -ForegroundColor Cyan
Invoke-CleanProcess $cmake $configureArguments $cleanEnvironment

Write-Host 'Building...' -ForegroundColor Cyan
Invoke-CleanProcess $cmake @('--build', $buildRoot, '--config', $Configuration, '--parallel') $cleanEnvironment

if (-not $SkipTests) {
    Write-Host 'Running offline tests...' -ForegroundColor Cyan
    Invoke-CleanProcess $ctest @('--test-dir', $buildRoot, '-C', $Configuration, '--output-on-failure') $cleanEnvironment
}

$exe = Join-Path $buildRoot "$Configuration\acheron.exe"
if (-not (Test-Path $exe)) {
    throw "Build completed but '$exe' was not produced."
}

if (-not $SkipDeploy) {
    $windeployqt = Join-Path $qtRootResolved 'bin\windeployqt.exe'
    Write-Host 'Deploying Qt runtime...' -ForegroundColor Cyan
    Invoke-CleanProcess $windeployqt @('--no-translations', '--no-opengl-sw', '--no-system-d3d-compiler', $exe) $cleanEnvironment
    Copy-Item -Path (Join-Path $curlRootResolved 'lib\*.dll') -Destination (Split-Path $exe) -Force
}

Write-Host ''
Write-Host 'Build succeeded.' -ForegroundColor Green
Write-Host "Executable: $exe"
Write-Host 'Voice: ON | RNNoise: ON | FFmpeg: ON'

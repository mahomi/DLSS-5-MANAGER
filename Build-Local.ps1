param([string]$RuntimeSource, [switch]$SkipChecks)
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskArtifacts = Join-Path $taskRoot 'artifacts'
$taskSeed = Join-Path $taskArtifacts 'runtime-seed/mod files'
$taskNative = Join-Path $taskArtifacts 'build/ScreenNative'
$taskDriverBuild = Join-Path $taskArtifacts 'build/SteamVR/driver'
$taskApp = Join-Path $taskArtifacts 'app-vr'
$taskDriver = Join-Path $taskArtifacts 'vr-feasibility/driver-live-flow'
$taskLogs = Join-Path $taskArtifacts 'build-logs'
$taskDotnet = Join-Path $env:ProgramFiles 'dotnet/dotnet.exe'
$taskOldPath = $env:Path
$taskOldLocation = Get-Location

function Copy-TaskTree([string]$Source, [string]$Destination) {
    if (!(Test-Path -LiteralPath $Source -PathType Container)) { throw "Missing runtime folder: $Source" }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    & robocopy.exe $Source $Destination /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP /XD __pycache__ /XF *.pyc | Out-Host
    if ($LASTEXITCODE -gt 7) { throw "Copy failed ($LASTEXITCODE): $Source" }
}
function Invoke-TaskBuild([string]$Directory, [string]$Command, [string]$Log) {
    Push-Location $Directory
    try {
        & $env:ComSpec /d /c $Command 2>&1 | Tee-Object -FilePath (Join-Path $taskLogs $Log) | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE): $Command" }
    } finally { Pop-Location }
}
function Stage-TaskSources([string]$Prefix, [string]$Destination) {
    $taskFiles = @(& git -C $taskRoot ls-files --cached --others --exclude-standard -- $Prefix)
    if ($LASTEXITCODE -ne 0) { throw 'Unable to enumerate source files.' }
    foreach ($taskFile in $taskFiles) {
        $taskRelative = $taskFile.Substring($Prefix.Length + 1)
        # Keep SDK/import libraries and Spout runtime assets; omit generated outputs.
        if ($taskRelative -match '\.(obj|exp|pdb|exe)$' -or $taskRelative -match '^nvngx\.dll') { continue }
        $taskDestination = Join-Path $Destination $taskRelative
        New-Item -ItemType Directory -Force -Path (Split-Path $taskDestination) | Out-Null
        Copy-Item -LiteralPath (Join-Path $taskRoot $taskFile) -Destination $taskDestination -Force
    }
}

try {
    Set-Location $taskRoot
    New-Item -ItemType Directory -Force -Path $taskLogs | Out-Null
    if ($RuntimeSource) {
        $taskImport = (Resolve-Path -LiteralPath $RuntimeSource).Path
        if ($taskImport -ne $taskSeed) { Copy-TaskTree $taskImport $taskSeed }
    }
    foreach ($taskRequired in @('native/runtime/python.exe', 'dlss 5/nvngx_dlssnr.dll', 'native/Spout.dll', 'native/SpoutDX.dll')) {
        if (!(Test-Path -LiteralPath (Join-Path $taskSeed $taskRequired))) {
            throw "Runtime seed missing: $taskRequired. Import the tested app's 'mod files' with -RuntimeSource once."
        }
    }
    if (!(Get-ChildItem -LiteralPath (Join-Path $taskSeed 'native/screen') -Filter *.py -File -Recurse)) { throw 'Capture controller Python source is missing.' }
    Stage-TaskSources 'ScreenNative' $taskNative
    Stage-TaskSources 'SteamVR/driver' $taskDriverBuild

    # vcvars64 expects a normal Windows PATH. This change is local to this process.
    $env:Path = 'C:\Windows\System32;C:\Windows;C:\Windows\System32\Wbem;C:\Windows\System32\WindowsPowerShell\v1.0'
    Invoke-TaskBuild $taskNative 'build.cmd' 'native.txt'
    Invoke-TaskBuild $taskDriverBuild 'build.cmd' 'driver.txt'
    if (!$SkipChecks) { Invoke-TaskBuild $taskNative 'build-fast-probe.cmd' 'fast-probe-build.txt' }
    $env:Path = $taskOldPath

    & $taskDotnet build (Join-Path $taskRoot 'DLSS 5 MANAGER.fsproj') -c Debug -o $taskApp 2>&1 | Tee-Object -FilePath (Join-Path $taskLogs 'app.txt') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw '.NET Debug build failed.' }
    Copy-TaskTree $taskSeed (Join-Path $taskApp 'mod files')
    foreach ($taskBinary in @('nvngx.dll', 'nvngx.dll_ns-forwarder.dll')) {
        Copy-Item -LiteralPath (Join-Path $taskNative $taskBinary) -Destination (Join-Path $taskApp "mod files/native/$taskBinary") -Force
    }

    # Package beside app-vr, matching the UI bridge's default relative driver path.
    New-Item -ItemType Directory -Force -Path (Join-Path $taskDriver 'bin/win64') | Out-Null
    Copy-Item -LiteralPath (Join-Path $taskDriverBuild 'bin/win64/driver_codex_live_flow.dll') -Destination (Join-Path $taskDriver 'bin/win64') -Force
    foreach ($taskFile in @('driver.vrdrivermanifest', 'manage-driver.ps1', 'register.cmd', 'unregister.cmd', 'restore-baseline.cmd')) {
        Copy-Item -LiteralPath (Join-Path $taskDriverBuild $taskFile) -Destination $taskDriver -Force
    }
    Copy-TaskTree (Join-Path $taskDriverBuild 'resources') (Join-Path $taskDriver 'resources')
    New-Item -ItemType Directory -Force -Path (Join-Path $taskDriver 'logs') | Out-Null
    foreach ($taskEye in @('left', 'right')) {
        $taskEngine = Join-Path $taskDriver "engine/$taskEye"
        New-Item -ItemType Directory -Force -Path (Join-Path $taskEngine 'libraries') | Out-Null
        foreach ($taskBinary in @('nvngx.dll', 'nvngx.dll_ns-forwarder.dll')) {
            Copy-Item -LiteralPath (Join-Path $taskNative $taskBinary) -Destination $taskEngine -Force
        }
        foreach ($taskBinary in @('Spout.dll', 'SpoutDX.dll')) {
            Copy-Item -LiteralPath (Join-Path $taskSeed "native/$taskBinary") -Destination $taskEngine -Force
        }
        Copy-Item -LiteralPath (Join-Path $taskSeed 'dlss 5/nvngx_dlssnr.dll') -Destination (Join-Path $taskEngine 'libraries') -Force
    }
    if (!$SkipChecks) {
        $taskSettings = Join-Path $taskLogs 'probe-settings.json'
        & (Join-Path $taskNative 'verify_fast_multipass.exe') --settings-check $taskSettings 2>&1 | Tee-Object -FilePath (Join-Path $taskLogs 'fast-settings.txt') | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Fast settings check failed.' }
        & (Join-Path $taskNative 'verify_fast_multipass.exe') $taskSettings 2>&1 | Tee-Object -FilePath (Join-Path $taskLogs 'fast-gpu.txt') | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Fast D3D12 GPU check failed.' }
        & $taskDotnet run --project (Join-Path $taskRoot 'Tests/VrUi/vr-ui-check.csproj') 2>&1 | Tee-Object -FilePath (Join-Path $taskLogs 'vr-ui.txt') | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'VR UI check failed.' }
    }
    Write-Output "Ready: $taskApp\DLSS 5 MANAGER.exe"
    Write-Output "VR package: $taskDriver"
    Write-Output 'Driver registration is a separate step; building does not change SteamVR registrations.'
} finally {
    $env:Path = $taskOldPath
    Set-Location $taskOldLocation
}

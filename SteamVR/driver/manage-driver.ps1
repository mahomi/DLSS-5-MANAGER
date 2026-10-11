param(
    [ValidateSet('Install','Remove','RestoreBaseline')][string]$Mode='Install',
    [switch]$DryRun
)
$ErrorActionPreference='Stop'
$taskDriverRoot=[IO.Path]::GetFullPath($PSScriptRoot)
$taskBaselineRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../driver-frame-output-sync'))
$taskTool='C:\Program Files (x86)\Steam\steamapps\common\SteamVR\bin\win64\vrpathreg.exe'
$taskRegistry=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'openvr/openvrpaths.vrpath'
if($DryRun){
    Write-Output "mode=$Mode new_driver=$taskDriverRoot baseline=$taskBaselineRoot"
    Write-Output 'Only these two exact paths may be added/removed; Virtual Desktop and other registrations are preserved.'
    Write-Output 'dry_run=true registry_changed=false'
    exit 0
}
function Invoke-TaskRegistry([string]$action,[string]$path){
    & $taskTool $action $path
    if($LASTEXITCODE -ne 0){throw "vrpathreg $action failed (exit $LASTEXITCODE)."}
}
try{
    $taskRunning=Get-Process -Name vrserver,vrcompositor,vrmonitor -ErrorAction SilentlyContinue
    if($taskRunning){throw 'Close VRChat and fully exit SteamVR before changing driver registration.'}
    if(!(Test-Path -LiteralPath $taskTool) -or !(Test-Path -LiteralPath $taskRegistry)){throw 'SteamVR registration tool or OpenVR registry is missing.'}
    $taskBefore=Get-Content -LiteralPath $taskRegistry -Raw | ConvertFrom-Json
    $taskRegistered=@($taskBefore.external_drivers)
    $taskHadNew=$taskRegistered -contains $taskDriverRoot
    $taskHadBaseline=$taskRegistered -contains $taskBaselineRoot
    if($Mode -eq 'Install'){
        foreach($taskRequired in @('bin/win64/driver_codex_live_flow.dll','engine/left/nvngx.dll','engine/right/nvngx.dll','engine/left/libraries/nvngx_dlssnr.dll','engine/right/libraries/nvngx_dlssnr.dll')){
            if(!(Test-Path -LiteralPath (Join-Path $taskDriverRoot $taskRequired))){throw "Build/runtime missing: $taskRequired"}
        }
        # Two proxy diagnostics cannot safely intercept the same HMD together.
        $taskSiblingRoots=@(Get-ChildItem -LiteralPath (Split-Path $taskDriverRoot) -Directory | Where-Object {$_.Name -like 'driver-*' -and $_.FullName -ne $taskDriverRoot -and $_.FullName -ne $taskBaselineRoot} | ForEach-Object {$_.FullName})
        foreach($taskPath in $taskRegistered){if($taskSiblingRoots -contains $taskPath){throw "Another diagnostic is registered: $taskPath. Remove it first; no registry changes made."}}
        New-Item -ItemType Directory -Force -Path (Join-Path $taskDriverRoot 'backups') | Out-Null
        $taskBackup=Join-Path $taskDriverRoot ('backups/openvrpaths-'+(Get-Date -Format 'yyyyMMdd-HHmmss')+'.json')
        Copy-Item -LiteralPath $taskRegistry -Destination $taskBackup
        try{
            if(!$taskHadNew){Invoke-TaskRegistry 'adddriver' $taskDriverRoot}
            if($taskHadBaseline){Invoke-TaskRegistry 'removedriver' $taskBaselineRoot}
        }catch{
            if($taskHadBaseline){& $taskTool adddriver $taskBaselineRoot | Out-Null}
            if(!$taskHadNew){& $taskTool removedriver $taskDriverRoot | Out-Null}
            throw
        }
        Write-Output 'Live Flow VR driver installed. Start SteamVR and VRChat, then open the separate VR Manager build.'
    }elseif($Mode -eq 'RestoreBaseline'){
        if(!(Test-Path -LiteralPath (Join-Path $taskBaselineRoot 'bin/win64/driver_codex_frame_output_sync.dll'))){throw 'Preserved baseline DLL is missing.'}
        if(!$taskHadBaseline){Invoke-TaskRegistry 'adddriver' $taskBaselineRoot}
        try{if($taskHadNew){Invoke-TaskRegistry 'removedriver' $taskDriverRoot}}catch{
            if(!$taskHadBaseline){& $taskTool removedriver $taskBaselineRoot | Out-Null}
            throw
        }
        Write-Output 'Preserved split-sync copy baseline restored.'
    }else{
        if($taskHadNew){Invoke-TaskRegistry 'removedriver' $taskDriverRoot}
        Write-Output 'Only the Live Flow VR driver was removed. Virtual Desktop remains registered.'
    }
    $taskAfter=Get-Content -LiteralPath $taskRegistry -Raw | ConvertFrom-Json
    $taskAfter | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $taskDriverRoot 'logs/registration-result.json') -Encoding UTF8
    exit 0
}catch{Write-Error $_ -ErrorAction Continue; exit 1}

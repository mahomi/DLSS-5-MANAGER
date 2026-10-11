param([switch]$FullResolution)
$ErrorActionPreference='Stop'
$taskRoot=Join-Path $PSScriptRoot 'tests/latest'
New-Item -ItemType Directory -Force -Path (Join-Path $taskRoot 'bin/win64'),(Join-Path $taskRoot 'logs') | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bin/win64/driver_codex_live_flow.dll') -Destination (Join-Path $taskRoot 'bin/win64/driver_codex_live_flow.dll')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'live_route_self_test.exe') -Destination $taskRoot
foreach($taskEye in @('left','right')){
    $taskEngine=Join-Path $taskRoot "engine/$taskEye"
    New-Item -ItemType Directory -Force -Path (Join-Path $taskEngine 'libraries') | Out-Null
    foreach($taskFile in @('nvngx.dll','nvngx.dll_ns-forwarder.dll','SpoutDX.dll','Spout.dll','libraries/nvngx_dlssnr.dll')){
        $taskSource=Join-Path $PSScriptRoot "engine/$taskEye/$taskFile"
        $taskDestination=Join-Path $taskEngine $taskFile
        if(!(Test-Path -LiteralPath $taskDestination) -or (Get-Item -LiteralPath $taskDestination).LastWriteTimeUtc -ne (Get-Item -LiteralPath $taskSource).LastWriteTimeUtc){
            Copy-Item -LiteralPath $taskSource -Destination $taskDestination
        }
    }
}
Push-Location $taskRoot
try{
    $taskArguments=@((Join-Path $taskRoot 'bin/win64/driver_codex_live_flow.dll'))
    if($FullResolution){$taskArguments+='--full-resolution'}
    & (Join-Path $taskRoot 'live_route_self_test.exe') @taskArguments *> (Join-Path $taskRoot 'logs/live-route-result.txt')
    $taskExit=$LASTEXITCODE
    Copy-Item -LiteralPath (Join-Path $taskRoot 'logs/live-route-result.txt') -Destination (Join-Path $PSScriptRoot 'logs/live-route-result.txt')
    Get-Content -LiteralPath (Join-Path $taskRoot 'logs/live-route-result.txt')
    exit $taskExit
}finally{Pop-Location}

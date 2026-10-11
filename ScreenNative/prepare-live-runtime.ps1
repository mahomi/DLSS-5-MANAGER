param(
    [Parameter(Mandatory=$true)][string]$ReleaseFolder,
    [Parameter(Mandatory=$true)][string]$AppDirectory
)
$ErrorActionPreference = 'Stop'
$releaseRoot = (Resolve-Path -LiteralPath $ReleaseFolder).Path
$appRoot = (Resolve-Path -LiteralPath $AppDirectory).Path
$payload = Join-Path $releaseRoot 'mod files'
foreach ($name in @('native/runtime/python.exe','native/screen/controller.py','dlss 5/nvngx_dlssnr.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $name))) { throw "Missing release payload: $name" }
}
# Preserve the built Manager and translations; only import release payload files.
Copy-Item -LiteralPath $payload -Destination $appRoot -Recurse -Force
$screen = Join-Path $appRoot 'mod files/native/screen'
$utf8 = New-Object System.Text.UTF8Encoding($false)

# The published 1.3.4 Python uses LF tags, while its public C++ source uses
# the earlier D5V3 tags with identical packed layouts. Keep both ends aligned.
$tags = @{
    HELLO_TAG='0x33563544'; TAKE_TAG='0x31504143'; FRAME_TAG='0x314D5246'; RESULT_TAG='0x3154554F';
    SHARED_TAG='0x494D4853'; SHARED_OK='0x4B434153'; MOTION_TAG='0x53544F4D'; MOTION_OK='0x4B43414D';
    SCREEN_TAG='0x4F444E57'; SCREEN_OK='0x4B434157'; RESHAPE_TAG='0x5A534E52'; RESHAPE_OK='0x4B434152';
    DESKTOP_TAG='0x31414444'; DESKTOP_OK='0x4B434144'; WINDOW_TAG='0x57434757'; WINDOW_OK='0x4B414757';
    EXPORT_TAG='0x5354554F'; EXPORT_OK='0x324B414F'; LUMA_TAG='0x59415247'; LUMA_OK='0x4B434147'
}
$transportPath = Join-Path $screen 'transport.py'
$transport = [IO.File]::ReadAllText($transportPath)
foreach ($tag in $tags.Keys) {
    $pattern = '(?m)^' + [regex]::Escape($tag) + ' = 0x[0-9A-Fa-f]+'
    if ([regex]::Matches($transport, $pattern).Count -ne 1) { throw "Unexpected release protocol: $tag" }
    $transport = [regex]::Replace($transport, $pattern, ($tag + ' = ' + $tags[$tag]))
}
[IO.File]::WriteAllText($transportPath, $transport, $utf8)

# Existing controller parameters still use the source worker's NS prefix.
# Fast Multi-pass itself is exclusively controlled by the live UI settings file.
foreach ($file in Get-ChildItem -LiteralPath $screen -Filter '*.py') {
    $source = [IO.File]::ReadAllText($file.FullName)
    if ($source.Contains('LF_')) {
        [IO.File]::WriteAllText($file.FullName, $source.Replace('LF_', 'NS_'), $utf8)
    }
}

# The public worker requires the motion payload even when it captures colour.
# Use the controller's existing luma/CPU guides; NVIDIA Optical Flow in the
# source worker can still replace those guides, with its normal CPU fallback.
$enginePath = Join-Path $screen 'engine.py'
$engine = [IO.File]::ReadAllText($enginePath)
$ownMotion = "own_motion = bool(st.takes_desktop) and st.settings.get('motion_backend', 'nvofa') != 'cpu'"
if (-not $engine.Contains($ownMotion)) { throw 'Unexpected release motion path' }
$engine = $engine.Replace($ownMotion, 'own_motion = False  # Public source worker requires motion payloads.')
[IO.File]::WriteAllText($enginePath, $engine, $utf8)

# Published host.py still limits initial passes to four; the source supports six.
$hostPath = Join-Path $screen 'host.py'
$hostSource = [IO.File]::ReadAllText($hostPath)
$hostSource = $hostSource.Replace('min(4, int(st.settings.get("passes", 1)))', 'min(6, int(st.settings.get("passes", 1)))')
[IO.File]::WriteAllText($hostPath, $hostSource, $utf8)

# Keep status recognition aligned with the public worker's diagnostic lines.
$motionPath = Join-Path $screen 'motion_state.py'
$motionSource = [IO.File]::ReadAllText($motionPath)
$motionSource = $motionSource.Replace('[ofa] NVIDIA optical flow unavailable:', '[nvofa] unavailable:').Replace('[ofa] running:', '[nvofa] active:')
[IO.File]::WriteAllText($motionPath, $motionSource, $utf8)
$preferencesPath = Join-Path $screen 'preferences.py'
$preferencesSource = [IO.File]::ReadAllText($preferencesPath)
$preferencesSource = $preferencesSource.Replace('NET_UP_LINES = (', 'NET_UP_LINES = ("[pure] direct feature 18 ready:", "[host] DLSS 5 feature 18 ready", ')
$preferencesSource = $preferencesSource.Replace('NET_DOWN_LINES = (', 'NET_DOWN_LINES = ("[host] NGX unavailable", "[host] no feature", ')
[IO.File]::WriteAllText($preferencesPath, $preferencesSource, $utf8)
$processorPath = Join-Path $screen 'processor.py'
$processorSource = [IO.File]::ReadAllText($processorPath)
$processorSource = $processorSource.Replace('_KEEP_TAGS = (', '_KEEP_TAGS = ("[pure]", "[host]", "[video]", "[nvofa]", "[fast-multipass]", ')
[IO.File]::WriteAllText($processorPath, $processorSource, $utf8)

foreach ($name in @('nvngx.dll', 'nvngx.dll_ns-forwarder.dll')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $appRoot ('mod files/native/' + $name)) -Force
}
Write-Output "Prepared Live Flow runtime: $appRoot"

# feedingfrenzy-nx (2026-10-26, from bejeweled3-nx's bj3_lightning_check.ps1): one deterministic run on the PC for the
# natives (docs/hand-natives.md): a fresh save, the test seed (srand, feedingfrenzy_debug_hooks.cpp), the frame clock
# and the pad script out\ff_lockstep_script.txt (Stage 1 played with the stick). -Verify turns on verify mode (every
# call of a leaf native checked); -NativesOff runs the generated code alone (the baseline); -Record records game frames
# -From .. -From + -Frames and keeps only their hashes under the tag (out\pc\captures\ffcheck_<Tag>.hash / .frames /
# .txt; the raw recording stays video_f<From>.rgba, which the next recording overwrites).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\ff_lockstep_check.ps1 -Tag off1 -NativesOff -Record
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [switch]$Verify,
    [switch]$NativesOff,
    [switch]$Record,
    [int]$From = 3600,
    [int]$Frames = 1800,
    [int]$Seconds = 150,
    [uint32]$Seed = 20261026
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $root
$script = (Get-Content (Join-Path $root 'out\ff_lockstep_script.txt') -Raw).Trim()
$userdata = Join-Path $root "out\pc\test_userdata\ffcheck_$Tag"
if (Test-Path $userdata) { Remove-Item -Recurse -Force $userdata }
New-Item -ItemType Directory -Force $userdata | Out-Null
$vars = @{
    REX_USER_DATA_ROOT = $userdata; REX_FEEDINGFRENZY_TEST_SEED = "$Seed"; REX_CLOCK_PER_SWAP_HZ = '60';
    REX_VBLANK_PER_SWAP = 'true'; REX_FEEDINGFRENZY_RENDERER = 'native'
}
if ($Verify) { $vars.REX_FEEDINGFRENZY_CPU_VERIFY = 'true' }
if ($NativesOff) { $vars.REX_FEEDINGFRENZY_CPU_NATIVES = 'false' }
foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
try {
    if ($Record) {
        powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -VideoFromFrame $From -VideoFrames $Frames -GameFps 60 -InputScript $script -Tail 2 | Select-Object -First 2
    } else {
        powershell -NoProfile -ExecutionPolicy Bypass -File tools\win\boot_test.ps1 -Seconds $Seconds -InputScript $script -Tail 2 | Select-Object -First 2
    }
} finally {
    foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
}
$log = (Get-ChildItem out\pc\logs\feedingfrenzy_*.log | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
"LOG $log"
if ($Record) {
    $cap = Join-Path $root 'out\pc\captures'
    python tools\compare_hashes.py --hash (Join-Path $cap "video_f$From.rgba")
    foreach ($e in 'hash', 'frames', 'txt') { Copy-Item -Force (Join-Path $cap "video_f$From.$e") (Join-Path $cap "ffcheck_$Tag.$e") }
    "kept out\pc\captures\ffcheck_$Tag.hash / .frames / .txt"
}

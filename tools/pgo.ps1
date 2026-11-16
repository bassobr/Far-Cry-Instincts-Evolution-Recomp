# Regenerates config/game_code.profdata, the profile the release build optimizes the recompiled code with (FCI_PGO).
# Run from the repository root after anything changes the generated code (recompiler, symbols, hle.txt):
#   powershell -File tools\pgo.ps1
# Builds the instrumented preset pgo-gen, plays each run below (menus and tutorial, the story's opening with the
# jeep and the truck, 22_DUNEB with water and fights) for its seconds with a fresh profile, writes the counters 5 s before its end and merges the runs. A stale
# profile is not an error: functions whose code changed are just optimized without it (the build stays correct, only
# slower).
param([int]$Seconds = 250)
$ErrorActionPreference = "Stop"
$root = (Get-Location).Path
if (-not (Test-Path "$root\assets\default.xbe") -and -not (Get-ChildItem "$root\assets\*.iso" -ErrorAction SilentlyContinue)) {
    throw "run from the repository root (assets\ missing)"
}
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User") +
    ";C:\Program Files (x86)\Microsoft Visual Studio\Installer"
[Environment]::CurrentDirectory = $root
& .\build.bat pgo-gen
if ($LASTEXITCODE -ne 0) { throw "build of pgo-gen failed" }

$dir = "$root\out\build\pgo-gen"
$profileDir = "$dir\data\hdd\partition1\UDATA\55530060\9FBDF33E7A0C"
# input script entries: the pattern ("<offset>:<button>[:<seconds>]") repeated every $Every seconds from $From to $To
function Repeat([int]$From, [int]$To, [int]$Every, [string[]]$Pattern) {
    $out = @()
    for ($t = $From; $t -lt $To; $t += $Every) {
        foreach ($p in $Pattern) { $o, $rest = $p -split ":", 2; $out += "$($t + [int]$o):$rest" }
    }
    $out -join ","
}
# the runs: seconds and environment of each (besides the fps limit and the profile output); their counters are merged
$runs = @(
    # title, profile with the virtual keyboard, main menu, tutorial: walk, the waterfall dive, swim, die, respawn
    @{ Seconds = $Seconds; Env = @{ "FCI_INPUT_SCRIPT" = "32:start,38:a,43:down,45:a,52:a,60:a," +
        (Repeat 70 $Seconds 10 @("0:lup:3", "3:rright:0.4", "4:lup:2.5", "7:a", "8:rleft:0.5", "9:rt:0.3")) } }
    # story from the menu: difficulty, 21_DUNEA opening (cutscene in the jeep, the truck's gun), the cutscene after it
    @{ Seconds = 160; Env = @{ "FCI_INPUT_SCRIPT" = "32:start,38:a,44:a,50:a," +
        (Repeat 70 160 10 @("0:rt:2", "2:rright:0.6", "4:rt:2", "6:rleft:0.6", "8:lup:1.5")) } }
    # 22_DUNEB: the opening at the jetty (boats, water), then swim, walk ashore, a pistol, fights
    @{ Seconds = 200; Env = @{ "FCI_START_LEVEL" = "22_DUNEB"; "FCI_SKIP_INTRO" = "1"; "FCI_INPUT_SCRIPT" = "40:a," +
        (Repeat 45 200 12 @("0:lup:5", "5:rright:0.5", "6:rt:1.5", "8:lup:3", "11:rleft:0.4")) } }
)
$raws = @()
for ($i = 0; $i -lt $runs.Count; $i++) {
    $run = $runs[$i]
    $raw = "$dir\game_code_$i.profraw"
    Remove-Item -Force $raw -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $profileDir -ErrorAction SilentlyContinue
    $vars = @{ "FCI_FPS_LIMIT" = "90"; "FCI_PGO_WRITE" = [string]($run.Seconds - 5); "LLVM_PROFILE_FILE" = $raw }
    foreach ($k in $run.Env.Keys) { $vars[$k] = $run.Env[$k] }
    foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }
    $p = Start-Process -FilePath "$dir\FarCryInstinctsEvolution.exe" -WorkingDirectory $dir -PassThru
    foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
    if (-not $p.WaitForExit($run.Seconds * 1000)) { Stop-Process -Id $p.Id -Force }
    if (-not (Test-Path $raw)) { throw "run $i wrote no profile (see $dir\fce.log, 'pgo:' line)" }
    $raws += $raw
}

$vs = & "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
& "$vs\VC\Tools\Llvm\x64\bin\llvm-profdata.exe" merge -o "$root\config\game_code.profdata" $raws
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata failed" }
"config\game_code.profdata written from $($raws.Count) run(s); build.bat release uses it"

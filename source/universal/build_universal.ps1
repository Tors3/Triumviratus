# =====================================================================================================================
#  build_universal.ps1 -- universal binary of Triumviratus 8.1 with PGO, in one shot, from this source folder.
#
#  Seven ISA variants, each in its own namespace Triumv_v<n>: by default one compilation unit for every .cpp of the
#  engine (wrappers with vns.h, ThinLTO; -Unity = one unit per variant, variant.cpp), entry point with cpuid (entry.cpp),
#  embedded network (triumv.rc), static C++ runtime. For every variant: instrumented build, deterministic training
#  (pgo_train_det.py on pgo_positions.epd, fixed `go nodes`: the same profile at every build), merge, optimised build.
#  Every variant trains with ITS OWN instructions if this CPU has them; otherwise vnni512 and avx512icl train with those
#  of avx512 and avxvnni with those of avx2, and without AVX-512 the variants 3-5 stay without a profile.
#
#  For releases built on several machines (every variant trained on a CPU that runs it) see the two-phase chain:
#  build_variants.ps1 (per machine) + pack_universal.ps1 (one executable), driven by build\release.ps1. The flags, the
#  compilation and the link are shared with them (ubuild_lib.ps1).
#
#  Needs: Visual Studio 2022 (or Build Tools) with the components "C++ Clang Compiler for Windows" and "MSBuild support
#  for LLVM (clang-cl) toolset", Windows SDK, Python 3, the network that triumv.rc embeds (..\nn-consilium.nnue: the
#  Consilium Plebis file under that name) in the source folder (one level up).
#
#  USE (PowerShell, no administrator needed):
#    .\build_universal.ps1                         # everything, release date = today
#    .\build_universal.ps1 -Day 20261010           # date of the id name line
#    .\build_universal.ps1 -NoPgo                  # without PGO (quick trial)
#    .\build_universal.ps1 -ExtraFlags "/clang:-mbranches-within-32B-boundaries"
#    .\build_universal.ps1 -Unity                  # one unit per variant (about 1% slower, see -Unity below)
#  Result: <Out>\Triumviratus_8.1_<Day>_universal.exe, SHA256SUMS.txt, verifica.txt (bench of every variant).
# =====================================================================================================================
param(
    [string]$Day = (Get-Date -Format yyyyMMdd),
    [string]$Out = "",
    [int]$Workers = [Environment]::ProcessorCount,
    [string]$ExtraFlags = "",
    [switch]$NoPgo,
    [switch]$Tune,
    [switch]$DevOptions,
    [switch]$NoStrictAliasing,
    # Empty = the "bench" of release.json (or "bench_possolver" with -PosSolver).
    [string]$ExpectedBench = "",
    # Separate files (DEFAULT since 09/10/2026): every .cpp of the engine is a unit of its own (wrapper with the
    # namespace of the variant, vns.h), compiled with -flto=thin and joined at link time, like the separate MSBuild build.
    # The universal with one unit per variant cost +0.98/+1.14% cycles/node against the separate build with the same
    # source; with separate files -0.28/-0.17% (xperf on the Xeon, pinned socket, 4 rounds, A/A noise <0.1%; z_un and
    # z_mtu in docs/audit_8.0/Z_RIVALUTAZIONE_VELOCITA.md). -Unity goes back to one unit (variant.cpp); -MultiTU is
    # still accepted.
    [switch]$MultiTU,
    [switch]$Unity,
    # Network for the TRAINING of the profile (10/10/2026, docs/audit_8.0/GRAFT_PASSEDREL_COSTO2.md section 5). Empty = the
    # embedded one. With a graft-block network the code of the blocks entered the profile (blocks removed from the
    # engine on 10/10/2026: today it only serves another network of the same format); -PgoNetShare 50 trains half the
    # workers with this one and half with the embedded one. The verification bench stays the embedded network's.
    [string]$PgoNet = "",
    [int]$PgoNetShare = 100,
    # One variant only (10/10/2026, xperf measurements): -Only 3 = only avx512, PGO with its own instructions, linked
    # with standalone.cpp (no cpuid entry). Writes <Out>\Triumviratus_8.1_<Day>_v<n>.exe. Separate files only
    # (default). For measuring, not for distributing.
    [int]$Only = 0,
    [int]$Jobs = [Environment]::ProcessorCount,    # compilations at once with separate files
    # Official release (14/10/2026): id name without date ("Triumviratus - 8.1", -DTRIUMV_OFFICIAL) and file
    # Triumviratus_8.1_windows_universal.exe. Without -Official it stays the dated pre-release.
    [switch]$Official,
    # Pos Solver (14/10/2026): analysis build (-DTRIUMV_POS_SOLVER: analysis mode always on), id name
    # "Triumviratus - 8.1 Pos Solver", file Triumviratus_8.1_PosSolver_windows_universal.exe. The bench is the analysis
    # mode one ("bench_possolver" of release.json, or -ExpectedBench).
    [switch]$PosSolver,
    # Training one variant at a time (28/10/2026, author: "use 3 processes so it does not interfere with the
    # tournament"): every variant gets all $Workers in turn, instead of all variants together with at least one worker
    # each (4 processes or more even with -Workers 3). Slower, same profiles. Use with -Jobs for the compilations.
    [switch]$SerialTrain
)
$MultiTU = -not $Unity
$ErrorActionPreference = "Stop"
$U   = $PSScriptRoot                               # ...\universal
$SRC = (Resolve-Path "$U\..").Path                 # engine source
. "$U\ubuild_lib.ps1"
if (-not $Out) { $Out = "$SRC\..\universal_out" }
$OBJ = "$Out\obj"; $PROF = "$Out\prof"
New-Item -ItemType Directory -Force $Out, $OBJ, $PROF | Out-Null
$Out = (Resolve-Path $Out).Path; $OBJ = (Resolve-Path $OBJ).Path; $PROF = (Resolve-Path $PROF).Path
$LOG = "$Out\build.log"
Set-Content -Encoding utf8 $LOG "build_universal.ps1 $(Get-Date)"
$rel = Get-Content -Raw "$U\release.json" | ConvertFrom-Json
$VER = $rel.version
if (-not $ExpectedBench) { $ExpectedBench = if ($PosSolver) { "$($rel.bench_possolver)" } else { "$($rel.bench)" } }
if (-not $ExpectedBench) { Fail "no expected bench: give -ExpectedBench (release.json has none for this flavour)" }

# --- tools --------------------------------------------------------------------------------------------------------
$tc = Find-Toolchain -NeedPgo:(-not $NoPgo)
$CXX = $tc.CXX; $LNK = $tc.LNK; $RC = $tc.RC; $PD = $tc.PD; $rt = $tc.rt; $py = $tc.py
$net = Get-EmbeddedNetPath
if (-not (Test-Path $net)) { Fail "missing the network $net (the file triumv.rc embeds)" }
Say "clang-cl: $CXX ($($tc.clang))"
Say "MSVC $($tc.msvc.Name), SDK $($tc.sdk.Name), workers $Workers, day $Day"

# --- CPU: which variants it runs ------------------------------------------------------------------------------------
$cpu = Get-CpuInfo
$can = $cpu.Can
$names = $VariantNames
Say "CPU $($cpu.Vendor) family $($cpu.Family): runnable variants '$can' (6 sse41, 1 avx2-nopext, 2 avx2, 7 avxvnni, 3 avx512, 4 vnni512, 5 avx512icl)"
if (-not $can.Contains('1')) { Fail "this CPU has no AVX2: the build (PGO training of the AVX2 variants) cannot run here" }

# --- flags --------------------------------------------------------------------------------------------------------
$base = Get-BaseFlags -Day $Day -DevOptions:$DevOptions -NoStrictAliasing:$NoStrictAliasing -ExtraFlags $ExtraFlags `
                      -Official:$Official -PosSolver:$PosSolver

# One unit per variant (-Unity): variant.cpp. $as = variant whose instructions to use (training on a CPU without $n's).
function Start-Variant($n, $objOut, $mode, $as, $profdata) {
    $a = $base + (VFlags $as) + (VTune $n) + @("-DTRIUMV_VID=$n")
    if ($mode -eq 'gen') { $a += @('-DTRIUMV_STANDALONE', '-fprofile-generate', '-DCLANG_PGO_GEN') }
    if ($mode -eq 'use') { $a += @("-fprofile-use=$profdata", '-Wno-profile-instr-out-of-date', '-Wno-profile-instr-unprofiled') }
    $a += @("$U\variant.cpp", "/Fo$objOut")
    # Start-Process joins the arguments with spaces and no quotes: those with spaces (paths) are quoted here.
    $a = $a | ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } }
    $p = Start-Process -FilePath $CXX -ArgumentList $a -NoNewWindow -PassThru -RedirectStandardOutput "$objOut.out" `
                       -RedirectStandardError "$objOut.err"
    Keep-Handle $p
}
if ($MultiTU) { $WRAP = New-UnitWrappers }

# --- common parts: fathom (C, ISA-independent), entry point, resources ----------------------------------------------
Say "common parts"
Build-CommonParts

# --- PGO ----------------------------------------------------------------------------------------------------------
$profs = @{}
if (-not $NoPgo) {
    $train = @{}
    foreach ($n in 1..7) {
        if ($can.Contains("$n")) { $train[$n] = $n }
        elseif ($n -eq 7 -and $can.Contains('2')) { $train[$n] = 2 }   # avx2 instructions, the variant's namespace
        elseif ($n -ge 4 -and $n -le 5 -and $can.Contains('3')) { $train[$n] = 3 }   # avx512 instructions, likewise
    }
    if ($Only) {
        if (-not $train.ContainsKey($Only)) { Fail "-Only ${Only}: variant not trainable on this CPU" }
        $train = @{ $Only = $train[$Only] }
    }
    Say "instrumented builds: $(($train.Keys | Sort-Object | ForEach-Object { "$($names[$_])<-$($names[$train[$_]])" }) -join ', ')"
    if ($MultiTU) {
        $gobjs = Compile-Units @($train.Keys | ForEach-Object { @{ n = $_; mode = 'gen'; as = $train[$_]; prof = '' } })
        foreach ($n in $train.Keys) { Link "$OBJ\g$n.exe" $gobjs[$n] -Gen }
    } else {
        $ps = foreach ($n in $train.Keys) { Start-Variant $n "$OBJ\g$n.obj" 'gen' $train[$n] '' }
        Wait-All $ps "instrumented build"
        foreach ($n in $train.Keys) { Link "$OBJ\g$n.exe" @("$OBJ\g$n.obj") -Gen }
    }
    Say "training ($Workers workers per variant, in parallel)"
    if ($PgoNet) {
        if (-not (Test-Path $PgoNet)) { Fail "training network not found: $PgoNet" }
        $env:PGO_EVALFILE = (Resolve-Path $PgoNet).Path
        $env:PGO_EVALFILE_SHARE = "$PgoNetShare"
        Say "training network: $($env:PGO_EVALFILE) ($PgoNetShare% of the workers)"
    }
    $perVar = if ($SerialTrain) { $Workers } else { [Math]::Max(1, [int]($Workers / $train.Count)) }
    $ps = foreach ($n in $train.Keys) {
        Remove-Item -Recurse -Force "$PROF\v$n" -ErrorAction SilentlyContinue; New-Item -ItemType Directory "$PROF\v$n" | Out-Null
        $env:LLVM_PROFILE_FILE = "$PROF\v$n\p_%p.profraw"
        $p = Keep-Handle (Start-Process -FilePath $py -ArgumentList @("`"$U\pgo_train_det.py`"", "`"$OBJ\g$n.exe`"", '0', "`"$U\pgo_positions.epd`"",
                      '200', '--workers', "$perVar") -NoNewWindow -PassThru `
                      -WorkingDirectory $OBJ -RedirectStandardOutput "$OBJ\train$n.log" -RedirectStandardError "$OBJ\train$n.err")
        if ($SerialTrain) { Wait-All @($p) "training $($names[$n])"; Say "training $($names[$n]) done" } else { $p }
    }
    Remove-Item Env:LLVM_PROFILE_FILE -ErrorAction SilentlyContinue
    if (-not $SerialTrain) { Wait-All $ps "training" }
    Remove-Item Env:PGO_EVALFILE, Env:PGO_EVALFILE_SHARE -ErrorAction SilentlyContinue
    foreach ($n in $train.Keys) {
        $raw = @(Get-ChildItem "$PROF\v$n\*.profraw")
        if (-not $raw.Count) { Fail "no profile for variant $n" }
        & $PD merge -o "$PROF\v$n.profdata" @($raw | ForEach-Object FullName)
        if ($LASTEXITCODE -ne 0) { Fail "merge of profile $n" }
        $profs[$n] = "$PROF\v$n.profdata"
    }
    # Every variant keeps its own profile (06/11/2026): with separate files each variant is linked alone and the native
    # objects are joined afterwards (Build-Variant and Link-Universal in ubuild_lib.ps1). The single merged profile of
    # before cost the AVX variants +5% instructions per node.
    Say "profiles: $(($profs.Keys | Sort-Object | ForEach-Object { $names[$_] }) -join ', ')"
}

# --- optimised builds and link ----------------------------------------------------------------------------------------
Say "optimised builds"
$exe = if ($Official) { "$Out\Triumviratus_${VER}_windows_universal.exe" } else { "$Out\Triumviratus_${VER}_${Day}_universal.exe" }
if ($PosSolver) { $exe = $exe -replace "Triumviratus_${VER}_", "Triumviratus_${VER}_PosSolver_" }
if ($Only) {
    if (-not $MultiTU) { Fail "-Only needs the separate-files build (without -Unity)" }
    $exe = "$Out\Triumviratus_${VER}_${Day}_v$Only.exe"
    $sp1 = if ($profs.ContainsKey($Only)) { @{ n = $Only; mode = 'use'; as = $Only; prof = $profs[$Only] } }
           else { @{ n = $Only; mode = 'none'; as = $Only; prof = '' } }
    $pobjs = Compile-Units @($sp1)
    $null = Build-Variant $Only $pobjs[$Only] $exe
    $b = (Get-Bench $exe '' $OBJ).Nodes
    $vlines = @("{0,-12} bench {1,-8} {2}" -f $names[$Only], $b, $(if ($b -eq $ExpectedBench) { "ok" } else { "BENCH DIFFERS (expected $ExpectedBench)" }))
    Set-Content -Encoding utf8 "$Out\verifica.txt" $vlines
    $vlines | ForEach-Object { Say $_ }
    if ($b -ne $ExpectedBench) { Fail "bench differs" }
    Say "READY: $exe"
    return
}
if ($MultiTU) {
    $pobjs = Compile-Units @(1..7 | ForEach-Object {
        if ($profs.ContainsKey($_)) { @{ n = $_; mode = 'use'; as = $_; prof = $profs[$_] } }
        else { @{ n = $_; mode = 'none'; as = $_; prof = '' } } })
    $nat = @{}
    foreach ($n in 1..7) { $nat[$n] = Build-Variant $n $pobjs[$n] "$OBJ\s$n.exe" }
    Link-Universal $exe $nat
} else {
    $ps = foreach ($n in 1..7) {
        if ($profs.ContainsKey($n)) { Start-Variant $n "$OBJ\p$n.obj" 'use' $n $profs[$n] }
        else { Start-Variant $n "$OBJ\p$n.obj" 'none' $n '' }
    }
    Wait-All $ps "optimised build"
    Link $exe @("$OBJ\entry.obj", "$OBJ\p1.obj", "$OBJ\p2.obj", "$OBJ\p3.obj", "$OBJ\p4.obj", "$OBJ\p5.obj", "$OBJ\p6.obj",
         "$OBJ\p7.obj")
}

# --- verification -------------------------------------------------------------------------------------------------
$vlines = @()
$ok = $true
foreach ($n in 1..7) {
    if (-not $can.Contains("$n")) { $vlines += "{0,-12} not runnable on this CPU" -f $names[$n]; continue }
    $b = (Get-Bench $exe $names[$n] $OBJ).Nodes
    $tag = if ($b -eq $ExpectedBench) { "ok" } else { $ok = $false; "BENCH DIFFERS (expected $ExpectedBench)" }
    $pg = if ($profs.ContainsKey($n)) { "PGO" } else { "no PGO" }
    $vlines += "{0,-12} bench {1,-8} {2,-10} {3}" -f $names[$n], $b, $pg, $tag
}
$auto = Get-Bench $exe '' $OBJ
$vlines += "id name $($auto.IdName); on this CPU the variant $($names[$cpu.Best]) starts (in a console: line 'info string Build: universal')"
Set-Content -Encoding utf8 "$Out\verifica.txt" $vlines
(Get-Sha256 $exe) + " *" + (Split-Path $exe -Leaf) | Set-Content -Encoding ascii "$Out\SHA256SUMS.txt"
$vlines | ForEach-Object { Say $_ }
if (-not $ok) { Fail "a variant does not give the expected bench" }
Say "READY: $exe"

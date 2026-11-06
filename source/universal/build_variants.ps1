# =====================================================================================================================
#  build_variants.ps1 -- release chain, phase 1 (29/10/2026): the variants THIS CPU runs, each with its PGO profile
#  recorded with its own instructions, each verified by bench in a standalone executable.
#
#  What travels between machines is the PROFILE of each variant (the only part that needs the CPU), with a manifest
#  that pins the source, the flags, the compiler and the network it was recorded with. pack_universal.ps1 compiles
#  every variant with its own profile, links it alone as here and joins the native objects behind entry.cpp
#  (Build-Variant and Link-Universal, ubuild_lib.ps1). The standalone executables written here are the check that each
#  variant, built with its own profile, gives the expected bench: the universal executable runs the same code.
#
#  Run it through build\release.ps1 (download of the source and of the network, toolchain check). By hand:
#    .\build_variants.ps1 -Out D:\rel\zen4                    # every variant this CPU runs
#    .\build_variants.ps1 -Out D:\rel\ultra7 -Variants avxvnni # only some of them
#    .\build_variants.ps1 -Out D:\rel\t -NoPgo -Variants avx2  # quick check of the chain, no training
#  Result in <Out>: Triumviratus_<ver>_<day|official>_<variant>.exe, profiles\v<n>.profdata, manifest.json,
#  verifica.txt, SHA256SUMS.txt. Copy the whole folder to the machine that packs the universal executable.
# =====================================================================================================================
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Day = (Get-Date -Format yyyyMMdd),
    [switch]$Official,
    [switch]$PosSolver,
    [string]$ExpectedBench = "",          # empty = release.json
    [string[]]$Variants = @(),            # names or numbers (avx2,avx512 or "2 3"); empty = every variant this CPU runs
    [int]$Workers = [Environment]::ProcessorCount,
    [int]$Jobs = [Environment]::ProcessorCount,
    # Positions of pgo_positions.epd used by the training (200 = all, as build_universal.ps1). Fewer only to test the
    # chain: the manifest records it and pack_universal.ps1 refuses such profiles without -AllowTestProfiles.
    [int]$TrainPositions = 200,
    [switch]$NoPgo,
    # nnperft at this depth (0 = off) on three positions, for every variant: a build without TRIUMV_RELEASE (the release
    # build removes the diagnostic commands), no PGO, compared with a full refresh of the network at every evaluation.
    [int]$NnPerft = 0,
    [string]$ExtraFlags = "",
    [switch]$Tune,
    [switch]$NoStrictAliasing,
    [string]$Commit = "",                 # recorded in the manifest (build\release.ps1 passes it); empty = git of $SRC
    [string]$ObjDir = ""                  # objects and raw profiles; empty = <Out>\..\_obj\<name of Out>
)
$MultiTU = $true
$ErrorActionPreference = "Stop"
$U   = $PSScriptRoot
$SRC = (Resolve-Path "$U\..").Path
. "$U\ubuild_lib.ps1"
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if (-not $ObjDir) { $ObjDir = Join-Path (Split-Path $Out -Parent) ("_obj\" + (Split-Path $Out -Leaf)) }
$OBJ = $ObjDir; $PROF = "$OBJ\prof"
New-Item -ItemType Directory -Force $OBJ, $PROF, "$Out\profiles" | Out-Null
$OBJ = (Resolve-Path $OBJ).Path; $PROF = (Resolve-Path $PROF).Path
$LOG = "$Out\build.log"
Set-Content -Encoding utf8 $LOG "build_variants.ps1 $(Get-Date)"
$rel = Get-Content -Raw "$U\release.json" | ConvertFrom-Json
$VER = $rel.version
if (-not $ExpectedBench) { $ExpectedBench = if ($PosSolver) { "$($rel.bench_possolver)" } else { "$($rel.bench)" } }
if (-not $ExpectedBench) { Fail "no expected bench: give -ExpectedBench (release.json has none for this flavour)" }

# --- tools, network, CPU --------------------------------------------------------------------------------------------
$tc = Find-Toolchain -NeedPgo:(-not $NoPgo)
$CXX = $tc.CXX; $LNK = $tc.LNK; $RC = $tc.RC; $PD = $tc.PD; $rt = $tc.rt; $py = $tc.py
Say "clang-cl: $CXX ($($tc.clang)); MSVC $($tc.msvc.Name), SDK $($tc.sdk.Name)"
$net = Get-EmbeddedNetPath
if (-not (Test-Path $net)) { Fail "missing the network $net (the file triumv.rc embeds; build\release.ps1 puts it there)" }
$netSha = Get-Sha256 $net
if ($netSha -ne $rel.network.sha256) { Fail "the network $net is not $($rel.network.file) of release.json (sha256 $netSha)" }
Say "network: $net = $($rel.network.file) (sha256 ok)"
$cpu = Get-CpuInfo
$can = $cpu.Can
Say "CPU $($cpu.Name) ($($cpu.Vendor) family $($cpu.Family)): runs '$can' = $((($can.ToCharArray()) | ForEach-Object { $VariantNames[[int]"$_"] }) -join ', ')"

$chosen = @()
$Variants = @(($Variants -join ',') -split '[,\s]+' | Where-Object { $_ })
if ($Variants.Count) {
    foreach ($v in $Variants) {
        $n = if ($v -match '^\d$') { [int]$v } else { @($VariantNames.Keys | Where-Object { $VariantNames[$_] -eq $v })[0] }
        if (-not $n) { Fail "unknown variant '$v' (names: $(($VariantNames.Values | Sort-Object) -join ', '))" }
        # A profile recorded with other instructions is what this chain exists to avoid.
        if (-not $can.Contains("$n")) { Fail "this CPU does not run $($VariantNames[$n]): its profile must be recorded on a CPU that does" }
        $chosen += $n
    }
} else {
    $chosen = @($can.ToCharArray() | ForEach-Object { [int]"$_" })
}
$chosen = @($chosen | Sort-Object -Unique)
if (-not $chosen.Count) { Fail "no variant to build" }
Say "variants: $(($chosen | ForEach-Object { $VariantNames[$_] }) -join ', ')"

$base = Get-BaseFlags -Day $Day -NoStrictAliasing:$NoStrictAliasing -ExtraFlags $ExtraFlags -Official:$Official -PosSolver:$PosSolver
$WRAP = New-UnitWrappers
Say "common parts"
Build-CommonParts

# --- PGO: instrumented builds, training one variant at a time with every worker, merge -------------------------------
$profs = @{}
if (-not $NoPgo) {
    Say "instrumented builds"
    $gobjs = Compile-Units @($chosen | ForEach-Object { @{ n = $_; mode = 'gen'; as = $_; prof = '' } })
    foreach ($n in $chosen) { Link "$OBJ\g$n.exe" $gobjs[$n] -Gen }
    foreach ($n in $chosen) {
        Say "training $($VariantNames[$n]) ($Workers workers, $TrainPositions positions)"
        Remove-Item -Recurse -Force "$PROF\v$n" -ErrorAction SilentlyContinue; New-Item -ItemType Directory "$PROF\v$n" | Out-Null
        $env:LLVM_PROFILE_FILE = "$PROF\v$n\p_%p.profraw"
        $p = Keep-Handle (Start-Process -FilePath $py -ArgumentList @("`"$U\pgo_train_det.py`"", "`"$OBJ\g$n.exe`"", '0',
                          "`"$U\pgo_positions.epd`"", "$TrainPositions", '--workers', "$Workers") -NoNewWindow -PassThru `
                          -WorkingDirectory $OBJ -RedirectStandardOutput "$OBJ\train$n.log" -RedirectStandardError "$OBJ\train$n.err")
        Remove-Item Env:LLVM_PROFILE_FILE -ErrorAction SilentlyContinue
        Wait-All @($p) "training $($VariantNames[$n])"
        $raw = @(Get-ChildItem "$PROF\v$n\*.profraw")
        if (-not $raw.Count) { Fail "no profile for $($VariantNames[$n])" }
        & $PD merge -o "$Out\profiles\v$n.profdata" @($raw | ForEach-Object FullName)
        if ($LASTEXITCODE -ne 0) { Fail "merge of the profile of $($VariantNames[$n])" }
        $profs[$n] = "$Out\profiles\v$n.profdata"
    }
}

# --- optimised standalone executables (each with its own profile) ---------------------------------------------------
Say "optimised builds"
$tag = if ($Official) { 'official' } else { $Day }
$pre = if ($PosSolver) { "Triumviratus_${VER}_PosSolver_${tag}" } else { "Triumviratus_${VER}_${tag}" }
$pobjs = Compile-Units @($chosen | ForEach-Object {
    if ($profs.ContainsKey($_)) { @{ n = $_; mode = 'use'; as = $_; prof = $profs[$_] } } else { @{ n = $_; mode = 'none'; as = $_; prof = '' } }
})
# Built as pack_universal.ps1 builds the variant inside the universal executable (Build-Variant, ubuild_lib.ps1): the
# executable verified here is the code the universal one runs.
$exes = @{}
foreach ($n in $chosen) { $exes[$n] = "$Out\${pre}_$($VariantNames[$n]).exe"; $null = Build-Variant $n $pobjs[$n] $exes[$n] }

# --- nnperft (optional): development build of the same variants -----------------------------------------------------
$perft = @{}
if ($NnPerft -gt 0) {
    Say "nnperft ${NnPerft}: development builds without PGO"
    $relBase = $base
    $base = Get-BaseFlags -Day $Day -DevOptions -NoStrictAliasing:$NoStrictAliasing -ExtraFlags $ExtraFlags -Official:$Official -PosSolver:$PosSolver
    $dobjs = Compile-Units @($chosen | ForEach-Object { @{ n = $_; mode = 'dev'; as = $_; prof = '' } }) -Standalone
    $base = $relBase
    $fens = @('startpos',
              'fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1',
              'fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1')
    foreach ($n in $chosen) {
        $dexe = "$OBJ\dev_v$n.exe"
        Link $dexe $dobjs[$n]
        $cmds = @(); foreach ($f in $fens) { $cmds += "position $f"; $cmds += "nnperft $NnPerft" }
        $t = Invoke-Engine $dexe $cmds '' $OBJ
        $lines = @([regex]::Matches($t, 'nnperft \d+ Nodes: (\d+) evals (\d+) verified (\d+) bad (\d+) keymism (\d+)'))
        $bad = 0; $verified = 0
        foreach ($m in $lines) { $verified += [int64]$m.Groups[3].Value; $bad += [int64]$m.Groups[4].Value + [int64]$m.Groups[5].Value }
        $ok = ($lines.Count -eq $fens.Count) -and ($bad -eq 0) -and ($verified -gt 0)
        $perft[$n] = [ordered]@{ depth = $NnPerft; positions = $lines.Count; verified = $verified; bad_or_keymism = $bad; ok = $ok }
        Say ("nnperft {0,-12} positions {1} verified {2} bad+keymism {3} {4}" -f $VariantNames[$n], $lines.Count, $verified, $bad,
             $(if ($ok) { 'ok' } else { 'FAILED' }))
    }
}

# --- verification -----------------------------------------------------------------------------------------------------
$vlines = @("Triumviratus $VER, phase 1 (build_variants.ps1), $(Get-Date -Format 'yyyy-MM-dd HH:mm')",
         "host $env:COMPUTERNAME, CPU $($cpu.Name) ($($cpu.Vendor) family $($cpu.Family)), $($tc.clang)",
         "expected bench $ExpectedBench, network $($rel.network.file) sha256 $netSha", "")
$allOk = $true
$vlist = @()
foreach ($n in $chosen) {
    $b = Get-Bench $exes[$n] '' $OBJ
    $ok = ($b.Nodes -eq $ExpectedBench) -and ($b.NetEmbedded -ne $false)
    if ($perft.ContainsKey($n) -and -not $perft[$n].ok) { $ok = $false }
    if (-not $ok) { $allOk = $false }
    $note = if ($b.NetEmbedded -eq $false) { "NETWORK FROM A FILE, NOT THE EMBEDDED ONE" }
            elseif ($b.Nodes -ne $ExpectedBench) { "BENCH DIFFERS (expected $ExpectedBench)" } else { "ok" }
    $pg = if ($profs.ContainsKey($n)) { "PGO native" } else { "no PGO" }
    $vlines += "{0,-12} bench {1,-9} {2,-11} {3}" -f $VariantNames[$n], $b.Nodes, $pg, $note
    $entry = [ordered]@{
        n = $n; name = $VariantNames[$n]; bench = $b.Nodes; ok = $ok; pgo = $profs.ContainsKey($n)
        exe = (Split-Path $exes[$n] -Leaf); exe_sha256 = (Get-Sha256 $exes[$n])
        profile = $(if ($profs.ContainsKey($n)) { "profiles/v$n.profdata" } else { $null })
        profile_sha256 = $(if ($profs.ContainsKey($n)) { Get-Sha256 $profs[$n] } else { $null })
        id_name = $b.IdName
    }
    if ($perft.ContainsKey($n)) { $entry.nnperft = $perft[$n] }
    $vlist += $entry
}
if (-not $Commit) {
    $Commit = "$(git -C $SRC rev-parse HEAD 2>$null)".Trim()
    if ($Commit -and "$(git -C $SRC status --porcelain --untracked-files=no -- . 2>$null)".Trim()) { $Commit += "+modified" }
}
$manifest = [ordered]@{
    format = 1; kind = 'variants'; created = (Get-Date -Format s); host = $env:COMPUTERNAME
    version = $VER; flavour = $(if ($PosSolver) { 'possolver' } else { 'game' }); official = [bool]$Official; day = $Day
    expected_bench = $ExpectedBench
    source = [ordered]@{ commit = $Commit; fingerprint = (Get-SourceFingerprint) }
    flags_fingerprint = (Get-FlagsFingerprint); flags_note = "$(($base) -join ' ')"
    toolchain = [ordered]@{ clang = $tc.clang; msvc = $tc.msvc.Name; sdk = $tc.sdk.Name }
    network = [ordered]@{ file = $rel.network.file; sha256 = $netSha }
    training = [ordered]@{ pgo = (-not $NoPgo); positions = $TrainPositions; workers = $Workers }
    cpu = [ordered]@{ name = $cpu.Name; vendor = $cpu.Vendor; family = $cpu.Family; runs = $can }
    variants = $vlist
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 "$Out\manifest.json"
Set-Content -Encoding utf8 "$Out\verifica.txt" $vlines
$sums = Get-ChildItem -Recurse -File $Out | Where-Object { $_.Name -notin 'SHA256SUMS.txt', 'build.log' } | Sort-Object FullName |
        ForEach-Object { (Get-Sha256 $_.FullName) + " *" + $_.FullName.Substring($Out.Length + 1).Replace('\', '/') }
Set-Content -Encoding ascii "$Out\SHA256SUMS.txt" $sums
$vlines | ForEach-Object { Say $_ }
if (-not $allOk) { Fail "a variant failed its check (verifica.txt)" }
Say "READY: $Out"

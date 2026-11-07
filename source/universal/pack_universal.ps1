# =====================================================================================================================
#  pack_universal.ps1 -- release chain, phase 2 (29/10/2026): the universal executable from the profiles that
#  build_variants.ps1 recorded on one or more machines (folders copied from one computer to another are fine).
#
#  Checks first that every input folder is intact (its SHA256SUMS.txt) and belongs to the same build as this source:
#  same source fingerprint, same compiler flags, same network, same clang, same version, flavour and expected bench.
#  Then takes for every variant the profile recorded on the CPU closest to the ones that variant is for (see "one
#  profile per variant" below), compiles every variant with its own profile, links it alone and joins the native
#  objects behind entry.cpp exactly as build_universal.ps1 does (Build-Variant and Link-Universal, ubuild_lib.ps1).
#  Finally runs the bench of every variant this CPU runs; the others are checked with verify_universal.ps1 on a CPU
#  that has them.
#
#  USE:
#    .\pack_universal.ps1 -From D:\rel\zen4, D:\rel\ultra7, D:\rel\xeon -Out D:\rel\dist
#    .\pack_universal.ps1 -From D:\rel\zen4 -Out D:\rel\dist -AllowMissing   # avxvnni without PGO (no CPU for it)
#  Result in <Out>: Triumviratus_<ver>_<day>_universal.exe (or _windows_universal.exe with -Official builds),
#  SHA256SUMS.txt, verifica.txt, manifest_universal.json, profiles\v<n>.profdata (the profile each variant was built with).
# =====================================================================================================================
param(
    [Parameter(Mandatory = $true)][string[]]$From,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Day = "",                   # empty = the day of the input folders (all the same), else today
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$AllowMissing,               # variants without a recorded profile are compiled without PGO
    [switch]$AllowTestProfiles,          # accept profiles trained on fewer positions than the release training
    [switch]$Force,                      # accept profiles recorded with another clang version
    # Must be the same as in phase 1 (the flags fingerprint checks it).
    [string]$ExtraFlags = "",
    [switch]$Tune,
    [switch]$NoStrictAliasing,
    [string]$ObjDir = ""
)
$MultiTU = $true
$ErrorActionPreference = "Stop"
$U   = $PSScriptRoot
$SRC = (Resolve-Path "$U\..").Path
. "$U\ubuild_lib.ps1"
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
if (-not $ObjDir) { $ObjDir = Join-Path (Split-Path $Out -Parent) ("_obj\" + (Split-Path $Out -Leaf)) }
$OBJ = $ObjDir
New-Item -ItemType Directory -Force $OBJ, "$Out\profiles" | Out-Null
$OBJ = (Resolve-Path $OBJ).Path
$LOG = "$Out\build.log"
Set-Content -Encoding utf8 $LOG "pack_universal.ps1 $(Get-Date)"
$rel = Get-Content -Raw "$U\release.json" | ConvertFrom-Json

# --- inputs ---------------------------------------------------------------------------------------------------------
$ins = @()
foreach ($f in $From) {
    if (-not (Test-Path "$f\manifest.json")) { Fail "$f has no manifest.json (not a build_variants.ps1 folder)" }
    $dir = (Resolve-Path $f).Path
    # Integrity after the copy between machines.
    foreach ($line in Get-Content "$dir\SHA256SUMS.txt") {
        if ($line -notmatch '^([0-9a-f]{64}) \*(.+)$') { continue }
        $p = Join-Path $dir ($Matches[2] -replace '/', '\')
        if (-not (Test-Path $p)) { Fail "$dir\SHA256SUMS.txt lists $($Matches[2]), which is missing" }
        if ((Get-Sha256 $p) -ne $Matches[1]) { Fail "$p does not match $dir\SHA256SUMS.txt (damaged copy?)" }
    }
    $m = Get-Content -Raw "$dir\manifest.json" | ConvertFrom-Json
    if ($m.kind -ne 'variants' -or $m.format -ne 1) { Fail "$dir\manifest.json: unknown format" }
    $ins += @{ dir = $dir; m = $m }
    Say "input ${dir}: host $($m.host), CPU $($m.cpu.name), variants $((@($m.variants | ForEach-Object { $_.name })) -join ', ')"
}
$m0 = $ins[0].m
foreach ($i in $ins) {
    foreach ($k in 'version', 'flavour', 'official', 'expected_bench', 'flags_fingerprint') {
        if ("$($i.m.$k)" -ne "$($m0.$k)") { Fail "$($i.dir): $k '$($i.m.$k)' differs from '$($m0.$k)' of $($ins[0].dir)" }
    }
    if ($i.m.source.fingerprint -ne $m0.source.fingerprint) { Fail "$($i.dir) was built from another source ($($i.m.source.commit) vs $($m0.source.commit))" }
    if ($i.m.network.sha256 -ne $m0.network.sha256) { Fail "$($i.dir) was built with another network" }
    if ($i.m.toolchain.clang -ne $m0.toolchain.clang -and -not $Force) { Fail "$($i.dir): $($i.m.toolchain.clang), not $($m0.toolchain.clang) (-Force to accept)" }
    if (-not $i.m.training.pgo -or $i.m.training.positions -lt 200) {
        if (-not $AllowTestProfiles) { Fail "$($i.dir) has test profiles (PGO $($i.m.training.pgo), $($i.m.training.positions) positions): -AllowTestProfiles to use them anyway" }
    }
}
$VER = $m0.version
$Official = [bool]$m0.official
$PosSolver = $m0.flavour -eq 'possolver'
$ExpectedBench = "$($m0.expected_bench)"
if (-not $Day) {
    $days = @($ins | ForEach-Object { $_.m.day } | Sort-Object -Unique)
    $Day = if ($days.Count -eq 1) { $days[0] } else { Get-Date -Format yyyyMMdd }
}

# --- this machine: same source, flags, network and compiler as the inputs ---------------------------------------------
$tc = Find-Toolchain
$CXX = $tc.CXX; $LNK = $tc.LNK; $RC = $tc.RC; $PD = $tc.PD; $rt = $tc.rt
Say "clang-cl: $CXX ($($tc.clang))"
if ($tc.clang -ne $m0.toolchain.clang -and -not $Force) { Fail "this machine has $($tc.clang), the profiles $($m0.toolchain.clang) (-Force to accept)" }
$fp = Get-SourceFingerprint
if ($fp -ne $m0.source.fingerprint) { Fail "the source in $SRC is not the one the profiles were recorded on ($($m0.source.commit))" }
$net = Get-EmbeddedNetPath
if (-not (Test-Path $net) -or (Get-Sha256 $net) -ne $m0.network.sha256) { Fail "the network $net is missing or is not the one of the profiles" }
$base = Get-BaseFlags -Day $Day -NoStrictAliasing:$NoStrictAliasing -ExtraFlags $ExtraFlags -Official:$Official -PosSolver:$PosSolver
if ((Get-FlagsFingerprint) -ne $m0.flags_fingerprint) { Fail "compiler flags differ from phase 1 (same -ExtraFlags, -Tune, -NoStrictAliasing?)" }
$cpu = Get-CpuInfo

# --- one profile per variant: recorded on the CPU closest to the ones the variant is for ------------------------------
# (06/11/2026) Among the folders that have the variant's profile, the one whose CPU has the lowest best variant: avx512
# from the Xeon (Skylake-SP) rather than from the Zen 4, avx2 from the Core Ultra rather than from an AVX-512 CPU. The
# first folder in -From order wins a tie. Before, the first folder with the profile won: the 04/11 pre-release, packed
# on the Zen 4, gave the Zen 4 profile to avx512 as well.
$isaRank = @{ 6 = 0; 1 = 1; 2 = 2; 7 = 3; 3 = 4; 4 = 5; 5 = 6 }
$pick = @{}
foreach ($n in 1..7) {
    $bestRank = 99
    foreach ($i in $ins) {
        $v = @($i.m.variants | Where-Object { $_.n -eq $n -and $_.pgo -and $_.ok }) | Select-Object -First 1
        if (-not $v) { continue }
        $runs = "$($i.m.cpu.runs)"
        $r = if ($runs) { $isaRank[[int]"$($runs[-1])"] } else { 98 }
        if ($v.pgo_sde) { $r += 50 }   # (07/11/2026) a profile recorded inside Intel SDE only when no CPU recorded one
        if ($r -lt $bestRank) {
            $bestRank = $r
            $pick[$n] = @{ path = Join-Path $i.dir ($v.profile -replace '/', '\'); host = $i.m.host; cpu = $i.m.cpu.name; bench = $v.bench
                           sde = [bool]$v.pgo_sde }
        }
    }
}
$missing = @(1..7 | Where-Object { -not $pick.ContainsKey($_) })
$hint = @{ 1 = 'any AVX2 CPU'; 2 = 'any AVX2+BMI2 CPU'; 3 = 'AVX-512 (Xeon, Zen 4)'; 4 = 'AVX-512 VNNI (Xeon Cascade Lake, Zen 4)'
           5 = 'AVX-512 VBMI2 (Zen 4, Ice Lake and later)'; 6 = 'any CPU (SSE4.1)'; 7 = 'AVX-VNNI without AVX-512 (Core Ultra 7, Intel 12th gen+)' }
if ($missing.Count) {
    $msg = "no profile for: $(($missing | ForEach-Object { "$($VariantNames[$_]) ($($hint[$_]))" }) -join '; ')"
    if (-not $AllowMissing) { Fail "$msg. Run build_variants.ps1 on such a CPU, or -AllowMissing to compile them without PGO" }
    Say "WARNING: $msg -> compiled without PGO"
}
foreach ($n in $pick.Keys | Sort-Object) {
    Say "profile $($VariantNames[$n]): $($pick[$n].host) ($($pick[$n].cpu))"
    # Profiles recorded before 06/11/2026 name the internal functions after the full path of the build folder: here
    # they would match nothing and those functions would be built without PGO, silently (see Compile-Units).
    if (@(& $PD show --all-functions $pick[$n].path | Select-String -SimpleMatch ':\' | Select-Object -First 1).Count) {
        Fail "$($pick[$n].path) names functions by folder path (recorded by the old build scripts): record it again with build_variants.ps1"
    }
    Copy-Item $pick[$n].path "$Out\profiles\v$n.profdata" -Force   # the profiles the executable was built with
}

# --- build: as build_universal.ps1, every variant with its own profile -------------------------------------------------
$WRAP = New-UnitWrappers
Say "common parts"
Build-CommonParts
Say "optimised builds"
$exe = if ($Official) { "$Out\Triumviratus_${VER}_windows_universal.exe" } else { "$Out\Triumviratus_${VER}_${Day}_universal.exe" }
if ($PosSolver) { $exe = $exe -replace "Triumviratus_${VER}_", "Triumviratus_${VER}_PosSolver_" }
$pobjs = Compile-Units @(1..7 | ForEach-Object {
    if ($pick.ContainsKey($_)) { @{ n = $_; mode = 'use'; as = $_; prof = "$Out\profiles\v$_.profdata" } } else { @{ n = $_; mode = 'none'; as = $_; prof = '' } } })
$nat = @{}
foreach ($n in 1..7) { $nat[$n] = Build-Variant $n $pobjs[$n] "$OBJ\s$n.exe" }
Link-Universal $exe $nat

# --- verification on this CPU ---------------------------------------------------------------------------------------
Say "bench of the variants this CPU runs"
$t = Test-Universal $exe $ExpectedBench $OBJ
$vlines = @("Triumviratus $VER universal, phase 2 (pack_universal.ps1), $(Get-Date -Format 'yyyy-MM-dd HH:mm')",
         "id name $($t.IdName); expected bench $ExpectedBench; source $($m0.source.commit); $($tc.clang)",
         "packed and checked on $env:COMPUTERNAME, CPU $($cpu.Name); here entry.cpp starts $($VariantNames[$cpu.Best])", "")
$allOk = $true
foreach ($r in $t.Rows) {
    $pfrom = if (-not $pick.ContainsKey($r.n)) { "no PGO" } elseif ($pick[$r.n].sde) { "PGO from $($pick[$r.n].host) (Intel SDE)" } else { "PGO from $($pick[$r.n].host)" }
    if ($r.runnable) {
        if (-not $r.ok) { $allOk = $false }
        $vlines += "{0,-12} bench {1,-9} {2,-28} {3}" -f $r.name, $r.bench, $pfrom, $r.note
    } else {
        $vlines += "{0,-12} {1,-15} {2,-28} to check on a CPU that runs it (verify_universal.ps1)" -f $r.name, '', $pfrom
    }
}
Set-Content -Encoding utf8 "$Out\verifica.txt" $vlines
(Get-Sha256 $exe) + " *" + (Split-Path $exe -Leaf) | Set-Content -Encoding ascii "$Out\SHA256SUMS.txt"
[ordered]@{
    format = 1; kind = 'universal'; created = (Get-Date -Format s); host = $env:COMPUTERNAME
    version = $VER; flavour = $m0.flavour; official = $Official; day = $Day; expected_bench = $ExpectedBench
    source = $m0.source; network = $m0.network; toolchain = [ordered]@{ clang = $tc.clang }
    exe = (Split-Path $exe -Leaf); exe_sha256 = (Get-Sha256 $exe)
    profiles = @(1..7 | ForEach-Object { [ordered]@{ name = $VariantNames[$_]; from = $(if ($pick.ContainsKey($_)) { $pick[$_].host } else { $null }) } })
    checked = @([ordered]@{ host = $env:COMPUTERNAME; cpu = $cpu.Name; rows = $t.Rows })
} | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 "$Out\manifest_universal.json"
$vlines | ForEach-Object { Say $_ }
if (-not $allOk) { Fail "a variant does not give the expected bench (verifica.txt)" }
Say "READY: $exe"

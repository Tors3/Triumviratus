# =====================================================================================================================
#  ubuild_lib.ps1 -- shared parts of the Windows universal build (29/10/2026). Dot-sourced by:
#    build_universal.ps1   one shot: every variant, PGO, one executable (development and quick releases)
#    build_variants.ps1    release chain, phase 1: the variants THIS CPU runs, PGO profile recorded with their own
#                          instructions, one verified executable per variant
#    pack_universal.ps1    release chain, phase 2: profiles from one or more machines -> the universal executable
#    verify_universal.ps1  bench of every variant a CPU runs, on any machine (no compiler needed)
#  The toolchain, the compiler flags, the CPU probe, the per-variant compilation and the link live here once, so the
#  scripts compile the same objects with the same flags.
#
#  The functions read these variables of the calling script (PowerShell dynamic scope): $U (this folder), $SRC (the
#  engine source), $OBJ (objects), $LOG, $Jobs, $MultiTU, $Tune, $base (Get-BaseFlags), $WRAP (New-UnitWrappers) and
#  the toolchain $CXX, $LNK, $RC, $PD, $rt (Find-Toolchain). PowerShell does not tell upper from lower case, so a caller
#  must not use $u, $src, $obj, $prof for anything else.
# =====================================================================================================================

# Variant numbers (TRIUMV_VID, entry.cpp) and the names TRIUMV_ISA accepts.
$VariantNames = @{ 1 = 'avx2-nopext'; 2 = 'avx2'; 3 = 'avx512'; 4 = 'vnni512'; 5 = 'avx512icl'; 6 = 'sse41'; 7 = 'avxvnni' }

function Say($m) { $l = "[$(Get-Date -Format HH:mm:ss)] $m"; Write-Host $l; if ($LOG) { Add-Content -Encoding utf8 $LOG $l } }
function Fail($m) { Say "ERROR: $m"; throw $m }

# --- toolchain: Visual Studio 2022 with clang-cl, lld-link, llvm-rc, llvm-profdata; Windows SDK; Python for training ---
function Find-Toolchain([switch]$NeedPgo) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { Fail "vswhere not found: Visual Studio 2022 is missing (see build\setup_toolchain.ps1)" }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
    if (-not $vs) { Fail "Visual Studio without the 'C++ Clang Compiler for Windows' component (see build\setup_toolchain.ps1)" }
    # The compiler (03/11/2026): the newest complete LLVM among $env:TRIUMV_LLVM, a standalone install
    # (%ProgramFiles%\LLVM: winget LLVM.LLVM, see build\setup_toolchain.ps1) and the one inside Visual Studio. clang 23
    # (LLVM 23.1.4) against the clang 19.1.5 of Visual Studio 17.14, same source and PGO, xperf 02/11/2026: cycles per
    # node -0.65% middlegame / -1.31% endgames on a Xeon (Skylake-SP), level on a Zen 4 laptop. Visual Studio still
    # provides the MSVC headers and libraries; $env:TRIUMV_LLVM points to another LLVM folder.
    $t = $null; $best = [version]'0.0'
    foreach ($root in @($env:TRIUMV_LLVM, "$env:ProgramFiles\LLVM", "$vs\VC\Tools\Llvm\x64")) {
        if (-not $root) { continue }
        $L = "$root\bin"
        $c = @{ CXX = "$L\clang-cl.exe"; LNK = "$L\lld-link.exe"; RC = "$L\llvm-rc.exe"; PD = "$L\llvm-profdata.exe" }
        if (@('CXX', 'LNK', 'RC', 'PD' | Where-Object { -not (Test-Path $c[$_]) }).Count) { continue }
        $c.rt = Get-ChildItem "$root\lib\clang\*\lib\windows\clang_rt.profile-x86_64.lib" -ErrorAction SilentlyContinue |
                Select-Object -First 1
        if (-not $c.rt -and $NeedPgo) { continue }
        $m = [regex]::Match("$((& $c.CXX --version)[0])", 'version (\d+\.\d+\.\d+)')
        $v = if ($m.Success) { [version]$m.Groups[1].Value } else { [version]'0.0' }
        if ($v -gt $best) { $best = $v; $t = $c }
    }
    if (-not $t) { Fail "no complete LLVM (clang-cl, lld-link, llvm-rc, llvm-profdata, clang_rt.profile) in TRIUMV_LLVM, $env:ProgramFiles\LLVM or Visual Studio (see build\setup_toolchain.ps1)" }
    if (-not $t.rt -and $NeedPgo) { Fail "missing clang_rt.profile-x86_64.lib (needed by PGO)" }
    $t.msvc = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
    $kits = (Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots" -ErrorAction SilentlyContinue).KitsRoot10
    if (-not $kits) { Fail "Windows SDK not found" }
    $t.sdk = Get-ChildItem "$kits\Lib" -Directory | Where-Object { Test-Path "$($_.FullName)\um\x64\kernel32.lib" } |
             Sort-Object Name -Descending | Select-Object -First 1
    $env:LIB = "$($t.msvc.FullName)\lib\x64;$($t.sdk.FullName)\um\x64;$($t.sdk.FullName)\ucrt\x64"
    $t.py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $t.py -and $NeedPgo) { Fail "Python 3 not found in PATH (the PGO training needs it)" }
    $t.clang = "$((& $t.CXX --version)[0])".Trim()
    return $t
}

# --- CPU: which variants it runs ------------------------------------------------------------------------------------
# Returns @{ Can = '6127345' (variant digits, sse41 first so that Can[-1] is the highest AVX one); Vendor; Family;
# Best = the variant entry.cpp picks here }. Needs $CXX and $OBJ.
function Get-CpuInfo {
    $probe = @'
#include <intrin.h>
#include <stdio.h>
#include <string.h>
__attribute__((target("xsave"))) static unsigned long long xcr0(void) { return _xgetbv(0); }
int main(void) {
  int r[4]; char vendor[13] = {0};
  __cpuid(r, 0); memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4); memcpy(vendor + 8, &r[2], 4);
  __cpuid(r, 1); int ecx1 = r[2], fam = ((r[0] >> 8) & 15) + ((r[0] >> 20) & 255);
  __cpuidex(r, 7, 0); unsigned ebx7 = r[1], ecx7 = r[2], sub7 = r[0];
  unsigned long long x = (ecx1 >> 27 & 1) ? xcr0() : 0;
  int avx = (x & 6) == 6, z = (x & 0xE6) == 0xE6;
  int avx2 = avx && (ebx7 >> 5 & 1) && (ebx7 >> 3 & 1);
  int pext = avx2 && (ebx7 >> 8 & 1);
  int f512 = z && (ebx7 >> 16 & 1) && (ebx7 >> 17 & 1) && (ebx7 >> 30 & 1) && (ebx7 >> 31 & 1);
  int vnni = f512 && (ecx7 >> 11 & 1);
  int icl = vnni && (ecx7 >> 1 & 1) && (ecx7 >> 6 & 1) && (ecx7 >> 12 & 1);
  int s41 = (ecx1 >> 19 & 1) && (ecx1 >> 23 & 1);
  int avxvnni = 0;
  if (sub7 >= 1) { __cpuidex(r, 7, 1); avxvnni = pext && (r[0] >> 4 & 1); }
  printf("%s%s%s%s%s%s%s %s %d\n", s41 ? "6" : "", avx2 ? "1" : "", pext ? "2" : "", avxvnni ? "7" : "", f512 ? "3" : "",
         vnni ? "4" : "", icl ? "5" : "", vendor, fam);
  return 0;
}
'@
    Set-Content -Encoding ascii "$OBJ\probe.c" $probe
    & $CXX /nologo /O2 "$OBJ\probe.c" "/Fe$OBJ\probe.exe" "/Fo$OBJ\probe.obj" | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "CPU probe did not compile" }
    $p = (& "$OBJ\probe.exe").Trim().Split(' ')
    $info = @{ Can = $p[0]; Vendor = $p[1]; Family = $p[2] }
    # Same rule as entry.cpp: PEXT is microcoded (slow) on AMD families 15h and 17h and on Hygon 18h.
    $best = if ($info.Can) { [int]("$($info.Can[-1])") } else { 0 }
    if ($best -eq 2 -and (($info.Vendor -eq 'AuthenticAMD' -and ($info.Family -eq '21' -or $info.Family -eq '23')) -or
                          ($info.Vendor -eq 'HygonGenuine' -and $info.Family -eq '24'))) { $best = 1 }
    $info.Best = $best
    $info.Name = "$((Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1).Name)".Trim()
    return $info
}

# --- flags (as build_pgo_clang_80.ps1 and the Xeon prototype) -------------------------------------------------------
function Get-BaseFlags([string]$Day, [switch]$DevOptions, [switch]$NoStrictAliasing, [string]$ExtraFlags, [switch]$Official,
                       [switch]$PosSolver) {
    $b = @('/c', '/nologo', '/std:c++17', '/EHsc', '/MT', '/O2', '/Oy', '/GS-', '/Gy', '/Gw', '/clang:-O3',
           '/clang:-ffp-contract=off', '-w', '-DNDEBUG', '-D_CONSOLE', '-DTRIUMV_EMBED_RESOURCE', '-DTRIUMV5_THREATS_INCR',
           '-DUSE_AVX2', '-DUSE_SSE41', '-DUSE_SSSE3', '-DUSE_SSE2', '-DUSE_SSE', '-DTRIUMV_RELEASE',
           "-DTRIUMV_RELEASE_DAY=$Day", "-I$SRC\fathom")
    # -DevOptions (20/10/2026): without TRIUMV_RELEASE, i.e. with the tuning options visible (and the diagnostic commands
    # such as nnperft, which TRIUMV_FROZEN removes from release builds).
    if ($DevOptions) { $b = @($b | Where-Object { $_ -ne '-DTRIUMV_RELEASE' }) }
    # Type-based alias analysis: gcc uses it by default, clang-cl for Windows does not. Measured 08/10/2026 evening
    # (xperf, 6 rounds): -1.06% cycles/node in the middlegame, -0.76% in endgames; checked with every VERIFY_* macro and gcc.
    if (-not $NoStrictAliasing) { $b += '/clang:-fstrict-aliasing' }
    # Loops aligned to 64 bytes: the one-unit-per-variant binary lost 2.2% of cycles against the separate builds in
    # avx512 with identical instructions; the alignment recovers 0.7 of it (xperf 08/10/2026).
    $b += '/clang:-falign-loops=64'
    if ($ExtraFlags) { $b += $ExtraFlags.Split(' ', [StringSplitOptions]::RemoveEmptyEntries) }
    if ($Official) { $b += '-DTRIUMV_OFFICIAL' }
    if ($PosSolver) { $b += '-DTRIUMV_POS_SOLVER' }
    return , $b
}
function VFlags($n) {
    switch ($n) {
        1 { @('/arch:AVX2', '-UUSE_PEXT', '-mno-bmi2', '-mno-avx512f') }
        2 { @('/arch:AVX2', '-DUSE_PEXT', '-mbmi2', '-mno-avx512f') }
        3 { @('/arch:AVX2', '-DUSE_PEXT', '-DUSE_AVX512', '-mavx512f', '-mavx512cd', '-mavx512bw', '-mavx512dq', '-mavx512vl', '-mbmi2',
              '-mno-avx512vnni') }
        4 { @('/arch:AVX2', '-DUSE_PEXT', '-DUSE_AVX512', '-DUSE_VNNI', '-mavx512f', '-mavx512cd', '-mavx512bw', '-mavx512dq', '-mavx512vl',
              '-mbmi2', '-mavx512vnni') }
        5 { @('/arch:AVX2', '-DUSE_PEXT', '-DUSE_AVX512', '-DUSE_VNNI', '-DUSE_AVX512ICL', '-mavx512f', '-mavx512cd', '-mavx512bw',
              '-mavx512dq', '-mavx512vl', '-mbmi2', '-mavx512vnni', '-mavx512vbmi', '-mavx512vbmi2', '-mavx512bitalg') }
        6 { @('-UUSE_AVX2', '-UUSE_PEXT', '-msse4.1', '-mssse3', '-mpopcnt', '-mno-avx', '-mno-avx2', '-mno-bmi2') }
        7 { @('/arch:AVX2', '-DUSE_PEXT', '-DUSE_VNNI', '-DUSE_AVXVNNI', '-mbmi2', '-mavxvnni', '-mno-avx512f') }
    }
}
# Tuning for the processors a variant is meant for. OFF by default (-Tune turns it on): on the Xeon
# -mtune=skylake-avx512 gave +0.5% cycles against the generic tuning (xperf 08/10/2026).
function VTune($n) {
    if (-not $Tune) { return @() }
    switch ($n) { 1 { @('/clang:-mtune=znver2') } 3 { @('/clang:-mtune=skylake-avx512') } 4 { @('/clang:-mtune=cascadelake') }
                  default { @() } }
}
# Fingerprint of the compiler flags, for checking that profiles from different machines belong to the same build: the
# release day and the source path are left out (the day is only a constant in the id name, the path differs per machine).
function Get-FlagsFingerprint {
    $all = @($base | Where-Object { $_ -notlike '-DTRIUMV_RELEASE_DAY=*' -and $_ -notlike "-I*fathom" })
    foreach ($n in 1..7) { $all += "|v$n|"; $all += (VFlags $n); $all += (VTune $n) }
    $all += "|multitu=$MultiTU"
    $bytes = [Text.Encoding]::UTF8.GetBytes(($all -join ' '))
    return ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)) -replace '-', '').ToLower()
}
# Content hash of the engine source (every file the build reads), the same on every machine for the same commit
# whatever the line endings git wrote: profiles may only be combined with the source they were recorded on.
function Get-SourceFingerprint {
    $latin = [Text.Encoding]::GetEncoding(28591)   # byte <-> char one to one
    $files = Get-ChildItem -Recurse -File $SRC -Include *.cpp, *.c, *.h, *.inc, *.rc, *.epd, *.py, *.ico |
             ForEach-Object { @{ f = $_.FullName; rel = $_.FullName.Substring($SRC.Length).TrimStart('\').ToLower().Replace('\', '/') } } |
             Where-Object { $_.rel -notmatch '^(x64|tools)/' } | Sort-Object { $_.rel }
    $sha = [Security.Cryptography.SHA256]::Create()
    $ms = New-Object IO.MemoryStream
    foreach ($f in $files) {
        $rel = [Text.Encoding]::UTF8.GetBytes($f.rel + "`n")
        $ms.Write($rel, 0, $rel.Length)
        # CRLF and LF checkouts hash the same.
        $norm = $latin.GetBytes($latin.GetString([IO.File]::ReadAllBytes($f.f)).Replace("`r", ""))
        $h = $sha.ComputeHash($norm)
        $ms.Write($h, 0, $h.Length)
    }
    return ([BitConverter]::ToString($sha.ComputeHash($ms.ToArray())) -replace '-', '').ToLower()
}

# PowerShell 5.1: with -NoNewWindow the returned process holds no handle, and if it ends before anyone opens it the exit
# code is lost (empty ExitCode, read as an error by Wait-All). Happened on the Ryzen 8845HS on 09/10/2026: the five
# compilations ended before Wait-All waited for them. Reading Handle right away keeps it open.
function Keep-Handle($p) { $null = $p.Handle; $p }
function Wait-All($procs, $what) {
    foreach ($p in $procs) { $p.WaitForExit() }
    foreach ($p in $procs) { if ($p.ExitCode -ne 0) { Fail "$what failed (exit code $($p.ExitCode)): see the .err files in $OBJ" } }
}

# Separate files: one wrapper for every .cpp that variant.cpp includes (same order, which is also the .vcxproj order,
# hence the same order of the global constructors). Returns the wrapper folder.
function New-UnitWrappers {
    $unitList = @(Select-String -Path "$U\variant.cpp" -Pattern '^#include "\.\./(.+\.cpp)"' |
                  ForEach-Object { $_.Matches[0].Groups[1].Value })
    $w = "$OBJ\mtu"
    New-Item -ItemType Directory -Force $w | Out-Null
    Remove-Item "$w\*.cpp" -ErrorAction SilentlyContinue
    $k = 0
    foreach ($unit in $unitList) {
        $k++
        $wname = '{0:D2}_{1}' -f $k, (($unit -replace '[\\/]', '_') -replace '\.cpp$', '')
        $srcFile = ((Resolve-Path "$SRC\$unit").Path) -replace '\\', '/'
        Set-Content -Encoding ascii "$w\$wname.cpp" @(
            '// generato da build_universal.ps1 -MultiTU',
            "#include `"$(($U -replace '\\', '/'))/prelude.h`"",
            "#include `"$(($U -replace '\\', '/'))/vns.h`"",
            'namespace TRIUMV_VNS {',
            "#include `"$srcFile`"",
            '}  // namespace TRIUMV_VNS')
    }
    return $w
}
# Compiles the variants of $specs (@{n; mode; as; prof}) as separate files, at most $Jobs compilations at once. mode is
# 'gen' (instrumented), 'use' (with the profile prof), anything else without a profile; 'as' = variant whose instructions
# to use. Returns, per variant, the objects in order. 'gen' and -Standalone add standalone.cpp (main of one variant).
function Compile-Units($specs, [switch]$Standalone) {
    $todo = New-Object System.Collections.Generic.List[object]
    $uobjs = @{}
    $wfiles = @(Get-ChildItem "$WRAP\*.cpp" | Sort-Object Name | ForEach-Object FullName)
    foreach ($sp in $specs) {
        $uobjs[$sp.n] = @()
        $files = $wfiles
        if ($sp.mode -eq 'gen' -or $Standalone) { $files += "$U\standalone.cpp" }
        foreach ($f in $files) {
            $o = "$OBJ\mtu_{0}{1}_{2}.obj" -f $sp.mode, $sp.n, [IO.Path]::GetFileNameWithoutExtension($f)
            $a = $base + (VFlags $sp.as) + (VTune $sp.n) + @("-DTRIUMV_VID=$($sp.n)", '-flto=thin')
            if ($sp.mode -eq 'gen') { $a += @('-fprofile-generate', '-DCLANG_PGO_GEN') }
            if ($sp.mode -eq 'use') { $a += @("-fprofile-use=$($sp.prof)", '-Wno-profile-instr-out-of-date', '-Wno-profile-instr-unprofiled') }
            $a += @($f, "/Fo$o")
            $a = $a | ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } }
            $todo.Add(@(, $a) + @($o))
            $uobjs[$sp.n] += $o
        }
    }
    $run = @()
    foreach ($t in $todo) {
        while (@($run | Where-Object { -not $_.HasExited }).Count -ge $Jobs) { Start-Sleep -Milliseconds 200 }
        $run += Keep-Handle (Start-Process -FilePath $CXX -ArgumentList $t[0] -NoNewWindow -PassThru `
                             -RedirectStandardOutput "$($t[1]).out" -RedirectStandardError "$($t[1]).err")
    }
    Wait-All $run "compilation (separate files)"
    return $uobjs
}
function Link($exe, $objs, [switch]$Gen) {
    $a = @('/nologo', "/OUT:$exe", '/SUBSYSTEM:CONSOLE', '/STACK:8388608', '/LARGEADDRESSAWARE') + $objs +
         @("$OBJ\tbprobe.obj", "$OBJ\triumv.res", 'advapi32.lib')
    if ($Gen) { $a += $rt.FullName } else { $a += @('/OPT:REF', '/OPT:ICF') }
    if ($MultiTU) { $a += "/opt:lldltojobs=$Jobs" }   # the objects are ThinLTO bitcode: the code is generated here
    # W3 (09/10/2026): no cmov -> branch conversion in the ThinLTO codegen (-0.15% middlegame, -0.29% endgames, z_w3)
    $a += '/mllvm:-x86-cmov-converter=false'
    $a += '/mllvm:-enable-ext-tsp-block-placement=true'   # 14/10/2026: ext-tsp adopted (xperf v14/et, with EVC8 -0.6% middlegame)
    # Arguments through a response file (29/10/2026): the seven variants are 150 objects, and with a long work folder the
    # command line passed the 32767 characters Windows allows ("file name or extension too long").
    $rsp = "$OBJ\link.rsp"   # one link at a time
    [IO.File]::WriteAllLines($rsp, [string[]]@($a | ForEach-Object { if ($_ -match ' ') { "`"$_`"" } else { $_ } }), (New-Object Text.UTF8Encoding $false))
    & $LNK "@$rsp" 2>&1 | Out-File -Append -Encoding utf8 $LOG
    if ($LASTEXITCODE -ne 0) { Fail "link of $exe failed" }
}
# Parts every executable shares: fathom (C, ISA-independent), the entry point, the resources (icon and network).
function Build-CommonParts {
    & $CXX /nologo /c /O2 /MT /arch:AVX2 /std:c11 -w "-I$SRC\fathom" "$SRC\fathom\tbprobe.c" "/Fo$OBJ\tbprobe.obj"
    if ($LASTEXITCODE -ne 0) { Fail "fathom" }
    & $CXX /nologo /c /std:c++17 /O2 /MT -w "$U\entry.cpp" "/Fo$OBJ\entry.obj"
    if ($LASTEXITCODE -ne 0) { Fail "entry.cpp" }
    Push-Location $U; & $RC /FO "$OBJ\triumv.res" triumv.rc; $rcOk = $LASTEXITCODE; Pop-Location
    if ($rcOk -ne 0) { Fail "resources (triumv.rc)" }
}
# The network file triumv.rc embeds (path relative to universal\), e.g. ..\nn-consilium.nnue.
function Get-EmbeddedNetPath {
    $m = Select-String -Path "$U\triumv.rc" -Pattern 'NNUE_DEFAULT\s+RCDATA\s+"([^"]+)"' | Select-Object -First 1
    if (-not $m) { Fail "triumv.rc has no NNUE_DEFAULT RCDATA line" }
    return [IO.Path]::GetFullPath((Join-Path $U ($m.Matches[0].Groups[1].Value -replace '/', '\')))
}

# --- running the engine ---------------------------------------------------------------------------------------------
# Sends $lines to $exe on stdin (from a file: the PowerShell 5.1 pipe sent a BOM and, to single-variant binaries, only
# the first line) and returns stdout + stderr. $isa sets TRIUMV_ISA (universal binary only).
function Invoke-Engine($exe, [string[]]$lines, [string]$isa = "", [string]$tmp = $env:TEMP) {
    $id = [Guid]::NewGuid().ToString('N').Substring(0, 8)
    $in = "$tmp\eng_$id.in"; $out = "$tmp\eng_$id.out"; $err = "$tmp\eng_$id.err"
    [IO.File]::WriteAllText($in, (($lines + @('quit')) -join "`n") + "`n", (New-Object Text.ASCIIEncoding))
    $old = $env:TRIUMV_ISA
    if ($isa) { $env:TRIUMV_ISA = $isa } else { Remove-Item Env:TRIUMV_ISA -ErrorAction SilentlyContinue }
    try {
        # The working directory is the temp folder: no network file next to it that the engine could prefer.
        $p = Start-Process -FilePath $exe -RedirectStandardInput $in -RedirectStandardOutput $out -RedirectStandardError $err `
                           -NoNewWindow -PassThru -Wait -WorkingDirectory $tmp
    } finally {
        if ($null -ne $old) { $env:TRIUMV_ISA = $old } else { Remove-Item Env:TRIUMV_ISA -ErrorAction SilentlyContinue }
    }
    $text = (Get-Content -Raw $out -ErrorAction SilentlyContinue) + (Get-Content -Raw $err -ErrorAction SilentlyContinue)
    Remove-Item $in, $out, $err -ErrorAction SilentlyContinue
    return "$text"
}
# Bench of one executable (one variant via TRIUMV_ISA for the universal one). Returns @{ Nodes; IdName; Variant and
# NetEmbedded, read from the start-up lines 'info string Build/Net', which the engine prints only when stdin is a
# console: from these scripts they stay empty, and a network file next to the executable shows as a different bench }.
function Get-Bench($exe, [string]$isa = "", [string]$tmp = $env:TEMP) {
    $t = Invoke-Engine $exe @('uci', 'bench') $isa $tmp
    $r = @{ Nodes = '?'; Variant = ''; NetEmbedded = $null; IdName = '' }
    if ($t -match 'Nodes searched\s*:\s*(\d+)') { $r.Nodes = $Matches[1] }
    if ($t -match 'info string Build: universal, variant (\S+)') { $r.Variant = $Matches[1] }
    if ($t -match 'info string Net: \S+ \(([^;]+);') { $r.NetEmbedded = ($Matches[1] -eq 'embedded') }
    if ($t -match '(?m)^id name (.+?)\r?$') { $r.IdName = $Matches[1] }
    return $r
}
function Get-Sha256($path) { (Get-FileHash -Algorithm SHA256 $path).Hash.ToLower() }
# Every variant of a universal executable that this CPU runs, forced with TRIUMV_ISA. With stdin redirected the engine
# is quiet at start-up (no 'info string Build: universal, variant X'), but entry.cpp writes "TRIUMV_ISA=<name> ..." on
# stderr when the CPU lacks the forced variant and starts the best one instead: a quick 'uci' tells which variants run
# here before the bench. Returns @{ Rows (per variant: n, name, runnable, bench, ok, note); IdName }.
function Test-Universal($exe, [string]$expected, [string]$tmp = $env:TEMP) {
    $rows = @()
    foreach ($n in 1..7) {
        $name = $VariantNames[$n]
        $probe = Invoke-Engine $exe @('uci') $name $tmp
        $ran = ($probe -match '(?m)^id name ') -and -not ($probe -match "(?m)^TRIUMV_ISA=$([regex]::Escape($name)) ")
        $row = [ordered]@{ n = $n; name = $name; runnable = [bool]$ran; bench = $null; ok = $false; note = 'not runnable on this CPU' }
        if ($ran) {
            $b = Get-Bench $exe $name $tmp
            $row.bench = $b.Nodes
            $row.ok = ($b.Nodes -eq $expected) -and ($b.NetEmbedded -ne $false)
            $row.note = if ($b.NetEmbedded -eq $false) { 'NETWORK FROM A FILE, NOT THE EMBEDDED ONE' }
                        elseif ($b.Nodes -ne $expected) { "BENCH DIFFERS (expected $expected)" } else { 'ok' }
        }
        $rows += $row
    }
    $id = Invoke-Engine $exe @('uci') '' $tmp
    return @{ Rows = $rows; IdName = $(if ($id -match '(?m)^id name (.+?)\r?$') { $Matches[1] } else { '' }) }
}

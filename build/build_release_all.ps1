# =============================================================================
#  Orchestratore build di RILASCIO Triumviratus.
#  Produce tutte le compilazioni e le raccoglie in _release\Triumviratus_<Version>\:
#    - Windows PGO  matrice completa (qui, via build_pgo_clang_<n>_*.ps1 -Arch all)
#    - Android      arm64 + dotprod (qui, via Git Bash + NDK -> build_release_native.sh)
#    - Linux        avx2 + avx512   (NON qui: gira build_release_native.sh su Linux/VM)
#
#  ⚠️ Prima del rilascio vero:
#     1) baka nei default di threads.cpp tutto cio' che e' stato gattato
#     2) metti la RETE FINALE accanto al sorgente della versione (vedi $cfg)
#
#  USO:  .\build_release_all.ps1                    # 7.0, Windows + Android
#        .\build_release_all.ps1 -SkipAndroid       # solo Windows
#        .\build_release_all.ps1 -Version 6.0       # vecchia release, catena 6.0
# =============================================================================
param(
  [ValidateSet("7.0","6.0")][string]$Version = "7.0",
  [string]$Ndk     = "C:\android-ndk-r27c",
  [switch]$SkipWindows,
  [switch]$SkipAndroid,
  # Bench atteso della matrice. Vuoto = canary NON armato: si verifica soltanto che
  # tutte le varianti diano lo STESSO bench (invariante node-identical), che e' gia'
  # il controllo che becca una variante rotta. Pinna il numero dopo la prima release.
  [string]$ExpectedBench = "",
  # Cartella di raccolta. Vuoto = _release\Triumviratus_<Version>. Serve per le build
  # datate di prova, che non devono sovrascrivere la release ufficiale.
  [string]$RelDir = ""
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# --- configurazione per versione ---------------------------------------------
# 🔴 MATRICE 7.0 (6/08/2026, VNNI separato il 10/08). Due assi indipendenti, ISA e VENDOR:
#   avx512icl   AVX512 + VBMI/VBMI2/BITALG   Ice Lake+, Sapphire Rapids, Zen5
#   vnni512     AVX512 F/BW/DQ/VL + VNNI     Cascade Lake, Ice Lake SP, Zen4
#   avx512      AVX512 F/BW/DQ/VL            Skylake-X, Xeon W-21xx (SENZA VNNI: quella
#                                            CPU non ce l'ha, e col vecchio target unico
#                                            moriva di SIGILL alla prima valutazione)
#   avx2        AVX2 + BMI2/PEXT             AMD Zen3+          (persp ON)
#   avx2-intel  come avx2                    Intel Haswell..Rocket Lake  (persp OFF)
#   avx2-nopext AVX2 senza PEXT              AMD Zen1/Zen2      (persp ON)
# NON esiste avx512-intel: su AVX-512 persp e' negativa per ENTRAMBI i vendor
# (misura 6/08, Zen4 ~-1,3% / Intel -1,17%), quindi la spegne il target per ISA e una
# sola build copre tutti. Lo split resta necessario solo su AVX2: +2,3% AMD, -1,17% Intel.
# Tutta la matrice e' node-identical => STESSO bench. Se non lo e', e' un bug.
$cfg = switch ($Version) {
  "7.0" { @{
      SrcDir   = "$root\Triumviratus_7"
      Builder  = "$root\build_pgo_clang_7_trann2.ps1"
      Net      = "Networks_Triumviratus_7\nn-legio-septima-v1.nnue"
      NetName  = "nn-legio-septima.nnue"
      Variants = @("avx512icl","vnni512","avx512","avx2","avx2-intel","avx2-nopext")
    } }
  "6.0" { @{
      SrcDir   = "$root\Triumviratus_6"
      Builder  = "$root\build_pgo_clang_6_trann1.ps1"
      Net      = $null                      # la 6.0 embedda la rete via .rc
      NetName  = "nn-rubicon-alea-v3.nnue"
      Variants = @("avx512","avx2")
    } }
}
$outDir = "$($cfg.SrcDir)\x64\Release"
$rel    = if ($RelDir) { if ([System.IO.Path]::IsPathRooted($RelDir)) { $RelDir } else { "$root\$RelDir" } }
          else { "$root\_release\Triumviratus_$Version" }
New-Item -ItemType Directory -Force -Path $rel | Out-Null
Write-Host "raccolta in: $rel" -ForegroundColor DarkGray

# --- 1. Windows PGO, matrice completa ---------------------------------------
if (-not $SkipWindows) {
  Write-Host "`n===== WINDOWS PGO ($($cfg.Variants -join ', ')) =====" -ForegroundColor Cyan
  $bp = @{ Arch = "all"; Release = $true; Name = "Triumviratus_$Version" }
  if ($cfg.Net) { $bp['Net'] = $cfg.Net }
  & $cfg.Builder @bp
  foreach ($a in $cfg.Variants) {
    $src = "$outDir\Triumviratus_${Version}_$a.exe"
    if (Test-Path $src) { Copy-Item $src $rel -Force; Write-Host "  raccolto $(Split-Path $src -Leaf)" -ForegroundColor Green }
    else { Write-Warning "manca $src" }
  }
  # 7.0: la rete si carica ACCANTO all'exe (EvalFileDefaultName in nnue/evaluate.h),
  # non e' embeddata nella .rc come nella 6.0 -> deve viaggiare con la release.
  if (Test-Path "$outDir\$($cfg.NetName)") { Copy-Item "$outDir\$($cfg.NetName)" $rel -Force }
  else { Write-Warning "manca la rete $($cfg.NetName) in $outDir" }
}

# --- 2. Android arm64 + dotprod (serve uname -> Git Bash; NDK cross-compile) ---
if (-not $SkipAndroid) {
  Write-Host "`n===== ANDROID (arm64 + dotprod) =====" -ForegroundColor Cyan
  # ⚠️ NON usare (Get-Command bash): su questa macchina risolve il bash di WSL
  # (WindowsApps\bash.exe), che senza distro installata fallisce. Serve GIT BASH.
  $bash = @(
    "$env:ProgramFiles\Git\bin\bash.exe",
    "${env:ProgramFiles(x86)}\Git\bin\bash.exe",
    "$env:LOCALAPPDATA\Programs\Git\bin\bash.exe"
  ) | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($bash) {
    Write-Host "  git-bash: $bash" -ForegroundColor DarkGray
    $rootU = $root -replace '\\','/'
    # path stile MSYS: C:\android-ndk-r27c -> /c/android-ndk-r27c
    $ndkMsys = "/" + $Ndk.Substring(0,1).ToLower() + ($Ndk.Substring(2) -replace '\\','/')
    & $bash -lc "cd '$rootU' && NDK='$ndkMsys' ./build_release_native.sh android"
    if ($LASTEXITCODE -ne 0) { Write-Warning "build Android fallita (vedi sopra)" }
    foreach ($b in @("triumviratus_android_dotprod","triumviratus_android")) {
      $src = "$outDir\$b"   # x64\Release, come le build Windows (fuori dal source-root)
      if (Test-Path $src) { Copy-Item $src $rel -Force; Write-Host "  raccolto $b" -ForegroundColor Green }
      else { Write-Warning "manca $src" }
    }
  } else {
    Write-Warning "bash non trovato sul PATH. Lancia Android a mano in Git Bash:`n  NDK=$($Ndk -replace ':','' -replace '\\','/' -replace '^','/') ./build_release_native.sh android"
  }
}

# --- 3. Licenza GPLv3 (motore Stockfish-derived) + checksum -> OBBLIGO GPL ---
$copying = "$($cfg.SrcDir)\COPYING"
if (Test-Path $copying) { Copy-Item $copying "$rel\COPYING" -Force; Write-Host "  COPYING (GPLv3) raccolto" -ForegroundColor Green }
else { Write-Warning "COPYING (GPLv3) MANCA in $($cfg.SrcDir) - la release NON puo' uscire senza licenza" }
# NB: line-ending LF, non CRLF. Le release notes dicono di verificare con `sha256sum -c` su
# Linux/macOS, e con CRLF quel comando fallisce ("No such file or directory": il \r finisce
# dentro al nome del file). Set-Content scriverebbe CRLF -> WriteAllText a mano.
$sums = Get-ChildItem $rel -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } |
  Get-FileHash -Algorithm SHA256 |
  ForEach-Object { "{0}  {1}" -f $_.Hash.ToLower(), (Split-Path $_.Path -Leaf) }
[System.IO.File]::WriteAllText("$rel\SHA256SUMS.txt", (($sums -join "`n") + "`n"),
                               (New-Object System.Text.UTF8Encoding $false))
Write-Host "  SHA256SUMS.txt scritto (LF)" -ForegroundColor Green

# --- 4. Canary: tutta la matrice deve benchare UGUALE ------------------------
# La 6.0 aveva un canary anti-B1 sul bench STANDALONE, perche' embeddava la rete nella
# .rc e una .rc sbagliata passava inosservata (2026-07-16: la release "funzionava" solo
# perche' il .nnue era copiato accanto). La 7.0 NON embedda: la rete viaggia accanto
# all'exe, quindi quel modo di fallire non esiste piu'. Resta l'invariante piu' forte:
# ISA e vendor cambiano COME si calcola, non COSA -> un bench diverso fra due varianti
# significa che una di esse sta cercando un albero diverso, cioe' un bug.
Write-Host "`n===== CANARY BENCH =====" -ForegroundColor Cyan
$benches = @{}
foreach ($a in $cfg.Variants) {
    $exePath = Join-Path $rel "Triumviratus_${Version}_$a.exe"
    # 🔴 10/08/2026: prima qui c'era `continue`, cioe' una variante MANCANTE veniva
    # saltata in silenzio e il canary passava sulle superstiti. Un controllo che si
    # accorge solo di cio' che trova non e' un controllo.
    if (-not (Test-Path $exePath)) { throw "CANARY FALLITO: manca la variante '$a' ($exePath). Build incompleta: NON rilasciare." }
    # `uci` e `isready` PRIMA del bench: senza, la 7.0 non risponde e $line esce null.
    $out = "uci`nisready`nbench`nquit" | & $exePath 2>&1 | Out-String
    $n = if ($out -match 'Nodes searched\s*:\s*(\d+)') { $Matches[1] } else { "?" }
    $benches[$a] = $n
    Write-Host ("  {0,-12} {1}" -f $a, $n)
}
$uniq = $benches.Values | Sort-Object -Unique
if ($uniq.Count -gt 1) {
    throw "CANARY FALLITO: la matrice non e' node-identical ($($uniq -join ' / ')). Una variante cerca un albero diverso: NON rilasciare."
}
if ($ExpectedBench -and $uniq -and $uniq[0] -ne $ExpectedBench) {
    throw "CANARY FALLITO: bench $($uniq[0]), atteso $ExpectedBench. Sorgente o rete non sono quelli previsti."
}
if (-not $ExpectedBench) {
    Write-Host "  [i] canary non armato: -ExpectedBench vuoto. Bench di questa matrice = $($uniq[0])" -ForegroundColor Yellow
    Write-Host "      Pinnalo al prossimo giro:  -ExpectedBench $($uniq[0])" -ForegroundColor Yellow
} else {
    Write-Host "  canary ok: tutta la matrice a $($uniq[0])" -ForegroundColor Green
}

# --- 4b. Canary ISA: nessuna variante contiene istruzioni fuori dal proprio target ---
# 🔴 PERCHE' SERVE, e perche' il canary bench NON basta (10/08/2026).
# Il 6/08 fu spedito un `avx512` con 44 `vpdpbusd` (VNNI). Su Skylake-X — che il VNNI
# NON ha, arriva con Cascade Lake — il motore risponde a `uci`, dice `readyok` e muore
# con SIGILL alla prima valutazione. Verificato su i9-7940X il 10/08.
# Il canary bench non lo ha visto per una ragione strutturale: e' un controllo A RUNTIME
# eseguito sulla macchina di BUILD, e quella macchina esegue tutto cio' che compila. Un
# binario troppo avanzato per un tester gira benissimo qui. Lo stesso vale per la verifica
# manuale: fu fatta su uno Xeon Gold 6242, che e' Cascade Lake e il VNNI ce l'ha.
# 🔑 Una macchina che possiede la feature non puo' falsificare l'assunzione che ci sia.
# Quindi il controllo deve essere STATICO: si guarda cosa c'e' nel binario, non se parte.
Write-Host "`n===== CANARY ISA =====" -ForegroundColor Cyan
$vsRoot   = if (Test-Path "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe") {
                & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
            } else { "C:\Program Files\Microsoft Visual Studio\2022\Community" }
$objdump  = "$vsRoot\VC\Tools\Llvm\x64\bin\llvm-objdump.exe"
# Mnemonici che NON devono comparire, per variante. `zmm` prende qualunque uso di AVX-512
# (e' nel nome dei registri); `\bpext\b` col confine di parola non collide con pextrb/w/d/q,
# che sono SSE4 e legittime. `vpdpbusd` prende anche `vpdpbusds`, che ne e' un prefisso.
$forbidden = [ordered]@{
    "avx512"      = 'vpdpbusd|vpdpwssd|vpermb|vperm[it]2b|vpcompress[bw]|vpexpand[bw]|vpshufbitqmb|vpopcnt[bw]|vpsh[lr]dv'
    "vnni512"     = 'vpermb|vperm[it]2b|vpcompress[bw]|vpexpand[bw]|vpshufbitqmb|vpopcnt[bw]|vpsh[lr]dv'
    "avx512icl"   = ''                      # gradino piu' alto: niente da vietare
    "avx2"        = 'zmm|vpdpbusd|vpdpwssd'
    "avx2-intel"  = 'zmm|vpdpbusd|vpdpwssd'
    "avx2-nopext" = 'zmm|vpdpbusd|vpdpwssd|\bpext\b|\bpdep\b'
}
if (-not (Test-Path $objdump)) {
    Write-Host "  [!] llvm-objdump non trovato ($objdump): canary ISA NON eseguito" -ForegroundColor Red
} else {
    foreach ($a in $cfg.Variants) {
        $pat = $forbidden[$a]
        if ($null -eq $pat) { Write-Host ("  {0,-12} nessun profilo ISA definito" -f $a) -ForegroundColor Yellow; continue }
        $exePath = Join-Path $rel "Triumviratus_${Version}_$a.exe"
        if (-not $pat) { Write-Host ("  {0,-12} ok (nessun vincolo)" -f $a) -ForegroundColor Green; continue }
        $hits = & $objdump -d $exePath 2>$null | Select-String -Pattern $pat
        if ($hits) {
            $mn = ($hits | ForEach-Object { ($_.Line -split '\s+' | Where-Object { $_ -match '^[a-z]' })[0] } |
                   Sort-Object -Unique) -join ', '
            throw "CANARY ISA FALLITO: '$a' contiene $($hits.Count) istruzioni fuori target ($mn). Su una CPU senza quella feature il motore muore di SIGILL: NON rilasciare."
        }
        Write-Host ("  {0,-12} ok, zero istruzioni fuori target" -f $a) -ForegroundColor Green
    }
}

Write-Host "`n===== RACCOLTA in $rel =====" -ForegroundColor Cyan
Get-ChildItem $rel | Sort-Object Name | Format-Table Name, @{N='MB';E={[math]::Round($_.Length/1MB,1)}} -AutoSize

Write-Host "PER LINUX (build su VM o box Linux con i sorgenti sincronizzati):" -ForegroundColor Yellow
Write-Host "  ./build_release_native.sh linux    # -> triumviratus_linux_avx2 / _avx512 (PGO, +5.7%)"
Write-Host "  poi scarica i 2 binari (via /tmp), mettili in $rel e RIGENERA SHA256SUMS.txt" -ForegroundColor Yellow
Write-Host "  (rilancia la sezione hash o: Get-FileHash -Algorithm SHA256 su $rel\* escluso SHA256SUMS.txt)" -ForegroundColor Yellow

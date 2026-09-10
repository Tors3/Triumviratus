# =============================================================================
#  bootstrap_release_laptop.ps1 — da zero a matrice di release, su una macchina nuova.
#
#  Clona il repo, ricostruisce il layout di lavoro che gli script di build si
#  aspettano, mette rete e libro al loro posto e lancia `build_release_all.ps1`.
#
#  USO (PowerShell, NON serve amministratore se il toolchain c'e' gia'):
#     .\bootstrap_release_laptop.ps1                      # tutto qui accanto
#     .\bootstrap_release_laptop.ps1 -NoBuild             # solo layout, per provare
#     .\bootstrap_release_laptop.ps1 -Only avx2,avx512    # sottoinsieme della matrice
#
#  🔴 DUE FILE NON SONO SU GITHUB e vanno portati a mano (chiavetta, cartella condivisa).
#     Non e' una dimenticanza: pesano 92 MB e 6 MB e non stanno in un repo.
#       nn-legio-septima.nnue        (92.417.127 byte, SHA256 verificato sotto)
#       UHO_2024_8mvs_+085_+094.epd  (il PGO training lo pretende, vedi sotto)
#     📌 METTILI NELLA STESSA CARTELLA DI QUESTO SCRIPT e non passare nulla: li trova
#        da solo. La rete viene cercata PER CONTENUTO — qualunque .nnue accanto allo
#        script il cui SHA256 combacia va bene, comunque si chiami. Con -Net / -Book
#        si possono indicare percorsi espliciti se stanno altrove.
#
#  ⚠️ PERCHE' NON BASTA CLONARE. Su GitHub i sorgenti stanno in `source/` e gli script
#     in `build/`, ma `build_release_all.ps1` cerca `Triumviratus_7\` e
#     `build_pgo_clang_7_trann2.ps1` ACCANTO A SE'. Il repo e' organizzato per essere
#     letto, l'albero di lavoro per essere compilato: questo script fa da ponte.
#
#  ⚠️ VNNI. La matrice completa fa girare il `bench` su OGNI variante come canary,
#     quindi va costruita su una CPU che sappia eseguirle tutte. Un Ryzen Zen 4 ce la
#     fa; uno Xeon Skylake-SP no — li' `vnni512` e `avx512icl` muoiono di istruzione
#     illegale DURANTE il controllo, e la matrice fallisce. Su quelle macchine:
#     -Only avx2,avx512.
# =============================================================================
param(
  [string]$Repo   = "https://github.com/Tors3/Triumviratus.git",
  # 🔴 Cartella DEDICATA, non "Desktop\Triumviratus": quella sul portatile e sul rig e'
  # gia' l'albero di lavoro (un repo git diverso da quello pubblico), e puntarci -Dest
  # significa fare `git pull` sul repo sbagliato e copiare sopra i sorgenti.
  [string]$Dest   = "$env:USERPROFILE\Desktop\Triumviratus_release",
  # Vuoti = si cercano accanto a questo script (vedi sopra).
  [string]$Net  = "",
  [string]$Book = "",
  [string[]]$Only = @(),
  [switch]$NoBuild,
  [switch]$SkipToolchain,
  # Android e' fuori di default: serve l'NDK, e su una macchina appena preparata non c'e'.
  [switch]$WithAndroid,
  # Firma bench attesa. Vuota = canary disarmato (sconsigliato).
  [string]$ExpectedBench = "240503"
)
$ErrorActionPreference = "Stop"

# La rete e' l'unico file di cui un errore silenzioso costerebbe giorni: una rete
# sbagliata compila, parte, gioca — e gioca peggio, senza dire niente. Quindi si
# verifica per contenuto, non per nome.
$NET_SHA256 = "b04e2835f538861bf26b7cc9e27c65911af420974f0ddab818ab0d3adf8e3ecc"
$NET_BYTES  = 92417127

function Step($m) { Write-Host "`n==== $m ====" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "  $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  $m" -ForegroundColor Yellow }

# --- 1. prerequisiti --------------------------------------------------------
Step "Prerequisiti"
$missing = @()
foreach ($c in @("git","python")) {
  if (-not (Get-Command $c -ErrorAction SilentlyContinue)) { $missing += $c }
}
# clang-cl non e' nel PATH: vive dentro l'installazione di VS. Si cerca col vswhere.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsOk = $false
if (Test-Path $vswhere) {
  $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset -property installationPath 2>$null
  if ($vsPath) { $vsOk = $true; Ok "Visual Studio + toolset ClangCL: $vsPath" }
}
if (-not $vsOk) { $missing += "Visual Studio 2022 + componenti Clang" }
foreach ($m in $missing) { Warn "MANCA: $m" }

if ($missing.Count -gt 0) {
  if ($SkipToolchain) { throw "Prerequisiti mancanti e -SkipToolchain attivo." }
  Warn "Installo il toolchain: serve una PowerShell come AMMINISTRATORE."
  Warn "Se questa non lo e', apri una PowerShell admin e lancia:"
  Warn "    Set-ExecutionPolicy -Scope Process Bypass -Force; .\build\setup_toolchain.ps1"
  Warn "poi rilancia questo script con -SkipToolchain."
  $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
             ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  if (-not $isAdmin) { throw "Servono i privilegi di amministratore per installare il toolchain." }
}

# --- 2. i due file che non stanno su GitHub ---------------------------------
# Si cercano ACCANTO A QUESTO SCRIPT quando non sono passati esplicitamente.
Step "Rete e libro"
$here = $PSScriptRoot
if (-not $here) { $here = (Get-Location).Path }   # se lanciato con . (dot-sourcing)

if (-not $Net) {
  # 🔑 Si cerca la rete PER CONTENUTO, non per nome: si scorrono i .nnue accanto allo
  # script e si prende quello il cui SHA256 combacia. Un file rinominato va bene lo
  # stesso, e un file col nome giusto ma contenuto sbagliato viene scartato — che e'
  # l'errore che conta, perche' una rete sbagliata compila, parte e gioca peggio in
  # silenzio. Il filtro sulla dimensione evita di calcolare l'hash di 90 MB a vuoto.
  $cands = @(Get-ChildItem -Path $here -Filter *.nnue -File -ErrorAction SilentlyContinue)
  foreach ($c in $cands) {
    if ($c.Length -ne $NET_BYTES) { continue }
    if ((Get-FileHash $c.FullName -Algorithm SHA256).Hash.ToLower() -eq $NET_SHA256) {
      $Net = $c.FullName; break
    }
  }
  if (-not $Net) {
    if ($cands.Count -eq 0) { throw "nessun .nnue accanto allo script ($here). Copiaci nn-legio-septima.nnue, oppure passa  -Net percorso\della\rete" }
    Write-Host "  .nnue trovati ma NESSUNO corrisponde alla rete attesa:" -ForegroundColor Red
    foreach ($c in $cands) { Write-Host ("    {0}  {1} byte" -f $c.Name, $c.Length) -ForegroundColor Red }
    throw "atteso SHA256 $NET_SHA256 ($NET_BYTES byte)."
  }
}
if (-not (Test-Path $Net)) { throw "rete non trovata: $Net" }
$netItem = Get-Item $Net
if ($netItem.Length -ne $NET_BYTES) {
  throw "la rete pesa $($netItem.Length) byte invece di ${NET_BYTES}: file sbagliato o troncato."
}
$sha = (Get-FileHash $Net -Algorithm SHA256).Hash.ToLower()
if ($sha -ne $NET_SHA256) {
  throw "SHA256 della rete non combacia.`n  atteso  $NET_SHA256`n  trovato $sha"
}
Ok "rete: $(Split-Path -Leaf $Net) - verificata per contenuto (SHA256 e dimensione)"

if (-not $Book) {
  # Il libro non ha un contenuto obbligato (sono posizioni di training per il PGO):
  # basta che sia un .epd. Si preferisce quello col nome atteso, altrimenti l'unico
  # presente; con piu' di uno si chiede, invece di sceglierne uno a caso.
  $bk = @(Get-ChildItem -Path $here -Filter *.epd -File -ErrorAction SilentlyContinue)
  $named = $bk | Where-Object { $_.Name -eq "UHO_2024_8mvs_+085_+094.epd" }
  if ($named)              { $Book = $named[0].FullName }
  elseif ($bk.Count -eq 1) { $Book = $bk[0].FullName }
  elseif ($bk.Count -eq 0) { throw "nessun .epd accanto allo script ($here). Copiaci UHO_2024_8mvs_+085_+094.epd, oppure passa  -Book percorso\del\libro" }
  else {
    Write-Host "  piu' di un .epd accanto allo script: indica quale con -Book" -ForegroundColor Red
    foreach ($b in $bk) { Write-Host "    $($b.Name)" -ForegroundColor Red }
    throw "libro ambiguo."
  }
}
if (-not (Test-Path $Book)) { throw "libro non trovato: $Book" }
# 🔴 NON `Get-Content -ReadCount 0 | Measure-Object -Line`: con -ReadCount 0 il
# contenuto esce come UN SOLO oggetto (l'array intero), e -Line conta le righe PER
# OGGETTO -> risponde 1 su qualunque file. Qui si scorre in streaming, che oltretutto
# non carica 16 MB in memoria.
$bookLines = 0
foreach ($l in [System.IO.File]::ReadLines((Resolve-Path $Book).Path)) { $bookLines++ }
if ($bookLines -lt 100) { throw "il libro $Book ha solo $bookLines righe: sembra troncato." }
Ok "libro: $(Split-Path -Leaf $Book) - $bookLines posizioni"

# --- 3. clone o aggiornamento ----------------------------------------------
Step "Repository"
# 🔴 CONTROLLO DI IDENTITA' DEL CLONE, e non e' pedanteria: il 09/09 il default di
# -Dest ha centrato l'albero di LAVORO gia' presente sul portatile — che e' un repo
# git DIVERSO da quello pubblico — e lo script gli ha fatto `git pull` addosso prima
# di accorgersi che `source\` non c'era. Un clone si riconosce dal REMOTE, non dal
# fatto che esista una .git.
function Repo-Matches($dir, $want) {
  $u = (git -C $dir remote get-url origin 2>$null)
  if (-not $u) { return $false }
  $n = { param($s) ($s -replace '\.git$','') -replace '/+$','' }
  return (& $n $u.Trim()).ToLower() -eq (& $n $want).ToLower()
}
if (Test-Path "$Dest\.git") {
  if (-not (Repo-Matches $Dest $Repo)) {
    throw @"
$Dest e' un repo git, ma il suo 'origin' NON e' $Repo
  origin trovato: $(git -C $Dest remote get-url origin 2>$null)
Quasi certamente e' il tuo albero di lavoro, non un clone di questo script.
Non lo tocco. Usa -Dest con una cartella dedicata, per esempio:
  -Dest "`$env:USERPROFILE\Desktop\Triumviratus_release"
"@
  }
  Ok "clone verificato in ${Dest}: aggiorno"
  git -C $Dest pull --ff-only
} else {
  if ((Test-Path $Dest) -and (Get-ChildItem $Dest -Force | Measure-Object).Count -gt 0) {
    throw "$Dest esiste e non e' vuota, e non e' un clone git. Scegli un altro -Dest."
  }
  git clone --depth 1 $Repo $Dest
}
Ok "commit: $(git -C $Dest rev-parse --short HEAD)  ($(git -C $Dest log -1 --format=%s))"

# --- 4. layout di lavoro ----------------------------------------------------
# Il repo separa `source/` e `build/`; gli script di build vogliono tutto alla radice
# con i sorgenti in `Triumviratus_7\`. Si copia invece di spostare, cosi' un `git pull`
# successivo continua a funzionare e la copia si rigenera.
Step "Layout di lavoro"
if (-not (Test-Path "$Dest\source"))  { throw "$Dest\source non esiste: il clone non ha il layout del repo pubblico." }
if (-not (Test-Path "$Dest\build"))   { throw "$Dest\build non esiste: il clone non ha il layout del repo pubblico." }
$src = "$Dest\Triumviratus_7"
# 🔴 /E e NON /MIR. /MIR specchia, cioe' CANCELLA nella destinazione tutto cio' che non
# sta nell'origine: puntato per sbaglio a una cartella di lavoro la svuota. Qui la
# destinazione e' generata da noi, quindi non serve specchiare e il rischio non vale
# la pulizia. I codici di robocopy sotto 8 sono successi (1 = file copiati, 2 = extra,
# 3 = entrambi); da 8 in su sono errori veri.
robocopy "$Dest\source" $src /E /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) {
  $why = switch ($LASTEXITCODE) {
    16 { "errore grave: origine inesistente o permessi insufficienti" }
    8  { "alcuni file non sono stati copiati" }
    default { "codice $LASTEXITCODE" }
  }
  throw "robocopy fallita ($LASTEXITCODE) copiando '$Dest\source' -> '$src': $why"
}
Ok "sorgenti -> Triumviratus_7\"

Copy-Item "$Dest\build\*.ps1" $Dest -Force
Copy-Item "$Dest\build\pgo_train.py" $Dest -Force
Ok "script di build -> radice"

# La rete serve in DUE posti, e non e' ridondanza:
#  - Networks_Triumviratus_7\ : e' il parametro -Net di build_pgo_clang_7_trann2.ps1
#  - accanto ai sorgenti      : il .rc la embedda come RCDATA (NNUE_DEFAULT), quindi
#                               deve esistere col nome esatto AL MOMENTO DELLA BUILD
New-Item -ItemType Directory -Force -Path "$Dest\Networks_Triumviratus_7" | Out-Null
Copy-Item $Net "$Dest\Networks_Triumviratus_7\nn-legio-septima-v1.nnue" -Force
Copy-Item $Net "$src\nn-legio-septima.nnue" -Force
Ok "rete -> Networks_Triumviratus_7\ e accanto ai sorgenti (per l'embedding RCDATA)"

# Il libro: build_pgo_clang_7_trann2.ps1 ha il percorso E IL NOME cablati (riga 98) e
# muore se manca, quindi il file va messo li' comunque si chiami in origine. Per il PGO
# non cambia nulla — servono posizioni rappresentative, non quel libro preciso — ma se
# il nome di partenza e' un altro lo si DICE, invece di lasciare sul disco un file che
# dichiara un contenuto diverso da quello che ha.
$bookDir  = "$Dest\OpeningBooks\uho_2024\UHO_2024_+085_+094"
$bookDest = "$bookDir\UHO_2024_8mvs_+085_+094.epd"
New-Item -ItemType Directory -Force -Path $bookDir | Out-Null
Copy-Item $Book $bookDest -Force
if ((Split-Path -Leaf $Book) -ne "UHO_2024_8mvs_+085_+094.epd") {
  Warn "il builder PGO vuole quel nome esatto: $(Split-Path -Leaf $Book) e' stato copiato COME"
  Warn "UHO_2024_8mvs_+085_+094.epd. Per il training va bene, ma il nome sul disco non descrive"
  Warn "piu' il contenuto - tienilo presente se un giorno cerchi quale libro fu usato."
} else {
  Ok "libro -> $bookDir"
}

# --- 5. build ---------------------------------------------------------------
# 🔴 IL SORGENTE CLONATO CORRISPONDE AL CANARY ATTESO? Il 09/09 il repo pubblico era
# fermo a un commit PRECEDENTE al bake delle costanti del blend eval: la matrice si
# sarebbe costruita per ore per poi fallire il canary alla fine — o, peggio, passarlo
# con un ExpectedBench disarmato e spedire il motore sbagliato. Il controllo costa una
# lettura di file e si fa PRIMA di accendere il compilatore.
# Dal 10/09 i bake da verificare sono DUE: il blend eval e ContHistMulti spento.
if ($ExpectedBench -eq "240503") {
  $bridge  = Get-Content "$src\nnue_bridge.cpp" -Raw
  $threads = Get-Content "$src\threads.cpp" -Raw
  $miss = @()
  if ($bridge  -notmatch 'g_ev_mat_base\s*=\s*84768')               { $miss += "blend eval (g_ev_mat_base = 84768)" }
  if ($threads -notmatch 'static bool g_conthist_multi\s*=\s*false') { $miss += "ContHistMulti spento (g_conthist_multi = false)" }
  if ($miss.Count -gt 0) {
    throw @"
Il sorgente clonato NON contiene: $($miss -join '; ').
  commit: $(git -C $Dest rev-parse --short HEAD) - $(git -C $Dest log -1 --format=%s)
Questo albero costruirebbe un motore diverso da quello con firma $ExpectedBench.
Committa e pubblica il bake sul repo, poi rilancia. Per costruire comunque il
sorgente cosi' com'e', passa -ExpectedBench con la firma che gli corrisponde.
"@
  }
  Ok "sorgente allineato al canary $ExpectedBench (blend eval e ContHistMulti spento presenti)"
}

if ($NoBuild) { Step "Layout pronto (-NoBuild)"; exit 0 }

Step "Matrice di release (PGO + ThinLTO)"
Write-Host "  Sono sei build PGO in sequenza: ore, non minuti." -ForegroundColor DarkGray
Write-Host "  Ogni variante e' costruita due volte (strumentata, poi ottimizzata sul profilo)." -ForegroundColor DarkGray
# 🔴 HASHTABLE, non array. `@array` splatta gli elementi come argomenti POSIZIONALI:
# "-Version" finisce come VALORE del primo parametro, e ValidateSet lo rifiuta ("-Version
# non appartiene al set 7.0;6.0"). Solo una hashtable splatta parametri con NOME.
# ⚠️ E niente -Net: build_release_all.ps1 non ha quel parametro, la rete la prende dalla
# sua mappa interna (Networks_Triumviratus_7\nn-legio-septima-v1.nnue), cioe' esattamente
# dove questo script l'ha messa.
$relArgs = @{ Version = "7.0" }
if ($ExpectedBench)   { $relArgs["ExpectedBench"] = $ExpectedBench }
if (-not $WithAndroid) { $relArgs["SkipAndroid"]  = $true }
if ($Only.Count) {
  $relArgs["Only"]   = $Only
  # Una matrice parziale non e' una release: lo script stesso si rifiuta di scrivere
  # nella cartella ufficiale, quindi le si da' una destinazione separata.
  $relArgs["RelDir"] = "_release\Triumviratus_7.0_parziale"
}
Push-Location $Dest
try { & "$Dest\build_release_all.ps1" @relArgs }
finally { Pop-Location }

Step "Fatto"
Write-Host "  I due canary sono passati: firma bench identica su tutta la matrice" -ForegroundColor Green
Write-Host "  e nessuna istruzione fuori dal target ISA di ciascun binario." -ForegroundColor Green

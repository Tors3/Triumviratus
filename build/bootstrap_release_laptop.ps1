# =============================================================================
#  bootstrap_release_laptop.ps1 -- da zero al binario UNIVERSALE di Triumviratus 8.0.
#
#  Scarica il repository e la rete, controlla il toolchain e lancia
#  source\universal\build_universal.ps1: cinque varianti ISA (avx2-nopext, avx2,
#  avx512, vnni512, avx512icl) in UN eseguibile che sceglie da solo all'avvio, con PGO.
#  Su una CPU che le ha tutte (Ryzen Zen 4, Ice Lake e successivi) ogni variante si
#  allena con le sue istruzioni: e' il motivo per costruire qui e non sullo Xeon.
#
#  USO (PowerShell, NON serve amministratore se il toolchain c'e' gia'):
#     Set-ExecutionPolicy -Scope Process Bypass -Force
#     .\bootstrap_release_laptop.ps1                    # tutto, data di rilascio = oggi
#     .\bootstrap_release_laptop.ps1 -Day 20261010      # data della riga id name
#     .\bootstrap_release_laptop.ps1 -NoBuild           # solo download e controlli
#  Risultato: <Dest>\out\Triumviratus_8.0_<Day>_universal.exe, SHA256SUMS.txt, verifica.txt
#
#  Sostituisce la versione della 7.0 (matrice di sei build separate): dalla 8.0 la
#  release e' un solo file. La rete (170 MB) si scarica dal pre-release v8.0 e si
#  verifica per contenuto; il libro non serve piu' (200 posizioni di training sono in
#  source\universal\pgo_positions.epd).
# =============================================================================
param(
  [string]$Repo   = "https://github.com/Tors3/Triumviratus",
  [string]$Branch = "main",
  # 🔴 Cartella DEDICATA, non "Desktop\Triumviratus": quella sul portatile e' gia'
  # l'albero di lavoro (un repo diverso da quello pubblico). Lo script la ricrea a ogni
  # giro scaricando il repo, quindi non deve mai coincidere con una cartella di lavoro.
  [string]$Dest    = "$env:USERPROFILE\Desktop\Triumviratus_universal",
  [string]$Day     = (Get-Date -Format yyyyMMdd),
  [int]$Workers    = [Environment]::ProcessorCount,
  [string]$NetUrl  = "https://github.com/Tors3/Triumviratus/releases/download/v8.0/nn-consilium.nnue",
  [string]$ExtraFlags = "",
  [string]$ExpectedBench = "222811",
  [switch]$NoBuild
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest e' 10 volte piu' lento con la barra di avanzamento

# La rete e' l'unico file di cui un errore silenzioso costerebbe giorni: una rete
# sbagliata compila, parte, gioca, e gioca peggio senza dire niente. Si verifica per contenuto.
$NET_SHA256 = "8fc004b0783ad94af50d6a544056d5285c7bdc48496b005575be51c79fa280c4"
$NET_BYTES  = 170288864

function Step($m) { Write-Host "`n==== $m ====" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "  $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  $m" -ForegroundColor Yellow }
function NetOk($p) {
  (Test-Path $p) -and ((Get-Item $p).Length -eq $NET_BYTES) -and
  ((Get-FileHash $p -Algorithm SHA256).Hash.ToLower() -eq $NET_SHA256)
}

# --- 1. prerequisiti --------------------------------------------------------
Step "Prerequisiti"
$missing = @()
if (-not (Get-Command python -ErrorAction SilentlyContinue)) { $missing += "Python 3" }
# clang-cl non e' nel PATH: vive dentro l'installazione di VS. Si cerca col vswhere.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = if (Test-Path $vswhere) {
  & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath 2>$null
} else { "" }
if ($vsPath) { Ok "Visual Studio + Clang: $vsPath" } else { $missing += "Visual Studio 2022 + componente 'C++ Clang Compiler for Windows'" }
foreach ($m in $missing) { Warn "MANCA: $m" }
if ($missing.Count -gt 0) {
  Warn "Per installarli: PowerShell come AMMINISTRATORE, poi"
  Warn "    Set-ExecutionPolicy -Scope Process Bypass -Force"
  Warn "    irm $Repo/raw/$Branch/build/setup_toolchain.ps1 -OutFile setup_toolchain.ps1; .\setup_toolchain.ps1"
  Warn "e rilancia questo script (in una PowerShell nuova, per il PATH aggiornato)."
  throw "Prerequisiti mancanti."
}

# --- 2. sorgente ------------------------------------------------------------
# Archivio zip del ramo invece di git clone: niente git da installare, e la cartella si
# ricrea da zero a ogni giro (nessun residuo di build vecchie, nessun pull sul repo sbagliato).
Step "Repository ($Repo, ramo $Branch)"
New-Item -ItemType Directory -Force $Dest | Out-Null
$zip = "$Dest\repo.zip"
Invoke-WebRequest "$Repo/archive/refs/heads/$Branch.zip" -OutFile $zip
$tmp = "$Dest\_estratto"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
Expand-Archive $zip $tmp
$root = (Get-ChildItem $tmp -Directory | Select-Object -First 1).FullName
$src  = "$Dest\source"
if (Test-Path $src) { Remove-Item -Recurse -Force $src }
Move-Item "$root\source" $src
Remove-Item -Recurse -Force $tmp, $zip
# Il sorgente pubblicato deve avere il binario universale: se manca, il repo e' fermo a
# prima della 8.0 universale e la build non avrebbe senso.
if (-not (Test-Path "$src\universal\build_universal.ps1")) {
  throw "Nel sorgente scaricato manca universal\build_universal.ps1: il repo non contiene ancora la 8.0 universale."
}
Ok "sorgente in $src"

# --- 3. rete ----------------------------------------------------------------
Step "Rete"
$net = "$src\nn-consilium.nnue"
$cache = "$Dest\nn-consilium.nnue"          # tenuta fra un giro e l'altro: 170 MB si scaricano una volta
$here = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
if (-not (NetOk $cache)) {
  # Prima si guarda accanto allo script (chiavetta, cartella condivisa), per contenuto.
  $local = Get-ChildItem $here -Filter *.nnue -File -ErrorAction SilentlyContinue |
           Where-Object { $_.Length -eq $NET_BYTES } | Where-Object { NetOk $_.FullName } | Select-Object -First 1
  if ($local) { Copy-Item $local.FullName $cache -Force; Ok "rete presa da $($local.FullName)" }
  else { Write-Host "  scarico la rete (170 MB) da $NetUrl"; Invoke-WebRequest $NetUrl -OutFile $cache }
}
if (-not (NetOk $cache)) { throw "la rete scaricata non corrisponde (atteso SHA256 $NET_SHA256, $NET_BYTES byte)" }
Copy-Item $cache $net -Force
Ok "rete verificata per contenuto (SHA256 e dimensione) -> accanto ai sorgenti, per l'incorporamento"

if ($NoBuild) { Step "Pronto (-NoBuild)"; exit 0 }

# --- 4. build ---------------------------------------------------------------
Step "Binario universale con PGO"
Write-Host "  Cinque varianti in parallelo: build strumentata, training, build ottimizzata, bench di ognuna." -ForegroundColor DarkGray
$b = @{ Day = $Day; Out = "$Dest\out"; Workers = $Workers; ExpectedBench = $ExpectedBench }
if ($ExtraFlags) { $b["ExtraFlags"] = $ExtraFlags }
& "$src\universal\build_universal.ps1" @b

Step "Fatto"
Get-Content "$Dest\out\verifica.txt" | ForEach-Object { Write-Host "  $_" }
Write-Host "`n  $Dest\out\Triumviratus_8.0_${Day}_universal.exe" -ForegroundColor Green

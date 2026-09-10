# =============================================================================
#  match_vs60_laptop.ps1 - 7.0 contro la 6.0 pubblicata, su un portatile.
#
#  Si arrangia da solo: scarica la 6.0 dalla release di GitHub, scarica fastchess,
#  verifica i due motori per firma bench e gioca il match.
#
#  USO:
#     .\match_vs60_laptop.ps1
#     .\match_vs60_laptop.ps1 -TC 25+0.25 -Games 2000 -Concurrency 14
#
#  🔴 QUALE VARIANTE CONTRO QUALE, e il nome inganna. L'`avx512` della 6.0 pubblicata
#     CONTIENE il VNNI (fu costruita il 16/07, prima che il difetto emergesse il 10/08),
#     mentre il nostro target `avx512` il VNNI lo esclude apposta per girare su
#     Skylake-X. Metterli uno contro l'altro regalerebbe alla 6.0 un gradino di ISA.
#     La controparte giusta e' il nostro `vnni512`. Su una CPU senza VNNI il confronto
#     non si puo' fare affatto in AVX-512: la 6.0 muore di istruzione illegale, e li'
#     si usa -Variant avx2 da entrambe le parti.
#
#  🔴 CONCORRENZA E MEMORIA. Ogni partita tiene VIVI DUE processi motore, anche se ne
#     pensa uno solo per volta: la memoria si conta su 2 x concurrency, non su
#     concurrency. Con hash 256 sono ~366 MB a processo. Lo script rifiuta di partire
#     se non ci sta - su Linux un match a hash 512 e' gia' stato ucciso dall'OOM killer
#     dopo zero partite, portandosi via anche altre applicazioni.
#
#  📌 La profondita' media va letta dai PGN a fine corsa (lo script la calcola): due
#     macchine allo stesso TC nominale sono gia' state trovate a 3 ply di distanza, e
#     senza quel numero un time control non descrive un regime.
# =============================================================================
param(
  # Il nostro binario. Vuoto = si cerca la variante scelta sotto _release.
  [string]$Engine7 = "",
  [ValidateSet("vnni512","avx512","avx2")][string]$Variant = "vnni512",
  [string]$TC          = "10+0.1",
  [int]$Concurrency    = 14,
  [int]$Hash           = 256,
  [int]$Games          = 3000,
  [string]$Book        = "",
  [string]$Dest        = "",
  # Firme bench attese: sono il controllo che stiamo misurando i motori che crediamo.
  [string]$Bench7 = "240503",
  [string]$Bench6 = "205566",
  [switch]$MemOk
)
$ErrorActionPreference = "Stop"
$ProgressPreference    = "SilentlyContinue"   # senza, Invoke-WebRequest impiega minuti per 90 MB

function Step($m) { Write-Host "`n==== $m ====" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "  $m" -ForegroundColor Green }
function Warn($m) { Write-Host "  $m" -ForegroundColor Yellow }

$here = $PSScriptRoot; if (-not $here) { $here = (Get-Location).Path }
if (-not $Dest) { $Dest = Join-Path $here "_match60" }
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# --- memoria: si controlla PRIMA di scaricare 90 MB e giocare per ore ------------
Step "Memoria"
$perProc = $Hash + 110
$needMB  = 2 * $Concurrency * $perProc
$freeMB  = [int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)
Write-Host ("  {0} processi x {1} MB = {2:N1} GB, liberi {3:N1} GB" -f (2*$Concurrency), $perProc, ($needMB/1024), ($freeMB/1024))
if ($needMB -gt ($freeMB * 0.85)) {
  $maxH = [math]::Floor(($freeMB * 0.85) / (2*$Concurrency)) - 110
  Warn "NON CI STA. Con concurrency $Concurrency il massimo e' hash ~$([math]::Floor($maxH/64)*64) MB,"
  Warn "oppure abbassa -Concurrency. Forzare comunque: -MemOk"
  if (-not $MemOk) { throw "memoria insufficiente" }
}
Ok "ok"

# --- il nostro motore -----------------------------------------------------------
Step "Triumviratus 7.0 ($Variant)"
if (-not $Engine7) {
  $cands = @(Get-ChildItem -Path $here -Recurse -Filter "Triumviratus_7.0_$Variant.exe" -File -ErrorAction SilentlyContinue)
  if ($cands.Count -eq 0) {
    $cands = @(Get-ChildItem -Path (Split-Path $here) -Recurse -Filter "Triumviratus_7.0_$Variant.exe" -File -ErrorAction SilentlyContinue)
  }
  if ($cands.Count -eq 0) { throw "non trovo Triumviratus_7.0_$Variant.exe. Costruiscila con bootstrap_release_laptop.ps1, o passa -Engine7." }
  $Engine7 = ($cands | Sort-Object LastWriteTime -Descending)[0].FullName
}
if (-not (Test-Path $Engine7)) { throw "motore 7.0 non trovato: $Engine7" }
Ok "uso $Engine7"

# --- la 6.0, scaricata dalla release --------------------------------------------
Step "Triumviratus 6.0 (download)"
$e60 = Join-Path $Dest "Triumviratus_6.0_avx512.exe"
if ($Variant -eq "avx2") { $e60 = Join-Path $Dest "Triumviratus_6.0_avx2.exe" }
if (Test-Path $e60) {
  Ok "gia' presente: $(Split-Path -Leaf $e60)"
} else {
  $asset = Split-Path -Leaf $e60
  $url   = "https://github.com/Tors3/Triumviratus/releases/download/v6.0/$asset"
  Write-Host "  scarico $asset (~90 MB) ..." -ForegroundColor DarkGray
  Invoke-WebRequest -Uri $url -OutFile $e60
  Ok "scaricato"
}

# --- fastchess, scaricato dalla sua release -------------------------------------
Step "fastchess (download)"
$fc = Join-Path $Dest "fastchess.exe"
if (Test-Path $fc) {
  Ok "gia' presente"
} else {
  # Si interroga l'API invece di cablare un nome di asset: fastchess li ha gia'
  # rinominati piu' volte, e un URL cablato marcisce in silenzio.
  $rel = Invoke-RestMethod "https://api.github.com/repos/Disservin/fastchess/releases/latest" -Headers @{ "User-Agent" = "triumviratus" }
  $a = $rel.assets | Where-Object { $_.name -match 'windows' -and $_.name -match '\.zip$' } |
       Sort-Object { if ($_.name -match 'avx2') { 0 } else { 1 } } | Select-Object -First 1
  if (-not $a) { throw "nessun asset Windows nella release $($rel.tag_name) di fastchess." }
  Write-Host "  $($rel.tag_name): $($a.name)" -ForegroundColor DarkGray
  $zip = Join-Path $Dest "fastchess.zip"
  Invoke-WebRequest -Uri $a.browser_download_url -OutFile $zip
  Expand-Archive -Path $zip -DestinationPath (Join-Path $Dest "fc_tmp") -Force
  $exe = Get-ChildItem (Join-Path $Dest "fc_tmp") -Recurse -Filter "fastchess*.exe" | Select-Object -First 1
  if (-not $exe) { throw "fastchess.exe non trovato dentro $($a.name)" }
  Copy-Item $exe.FullName $fc -Force
  Remove-Item $zip, (Join-Path $Dest "fc_tmp") -Recurse -Force
  Ok "installato"
}

# --- libro ----------------------------------------------------------------------
Step "Libro"
if (-not $Book) {
  $bk = @(Get-ChildItem -Path $here -Filter *.epd -File -ErrorAction SilentlyContinue)
  if ($bk.Count -eq 0) { throw "nessun .epd accanto allo script. Copiaci UHO_4060_v4.epd, o passa -Book." }
  $Book = ($bk | Sort-Object Length -Descending)[0].FullName
}
$n = 0; foreach ($l in [System.IO.File]::ReadLines((Resolve-Path $Book).Path)) { $n++ }
Ok "$(Split-Path -Leaf $Book) - $n posizioni"

# --- CANARY: i due motori sono quelli che crediamo? -----------------------------
# Un binario sbagliato gioca lo stesso e il risultato sembra buono: la firma bench e'
# l'unica verifica che non si puo' fraintendere. Qui coglie anche il SIGILL - su una
# CPU senza VNNI la 6.0 avx512 muore proprio qui, prima di sprecare ore di partite.
Step "Canary"
function Bench-Of($exe) {
  $out = ("uci`nisready`nbench`nquit" | & $exe 2>&1 | Out-String)
  if ($out -match "Nodes searched\s*:\s*(\d+)") { return $Matches[1] }
  return $null
}
$b7 = Bench-Of $Engine7
$b6 = Bench-Of $e60
if (-not $b7) { throw "la 7.0 non ha prodotto un bench: binario rotto o ISA non supportata da questa CPU." }
if (-not $b6) { throw "la 6.0 non ha prodotto un bench. Se e' l'avx512: contiene VNNI, e questa CPU non ce l'ha. Rilancia con -Variant avx2." }
if ($Bench7 -and $b7 -ne $Bench7) { throw "firma 7.0 inattesa: $b7 (attesa $Bench7). Non e' il motore bakato." }
if ($Bench6 -and $b6 -ne $Bench6) { throw "firma 6.0 inattesa: $b6 (attesa $Bench6)." }
Ok "7.0 bench $b7   |   6.0 bench $b6"

# --- match ----------------------------------------------------------------------
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$pgn   = Join-Path $Dest "vs60_$stamp.pgn"
$log   = Join-Path $Dest "vs60_$stamp.log"
Step "Match"
Write-Host "  $TC   hash $Hash   concurrency $Concurrency   $Games partite" -ForegroundColor DarkGray
Write-Host "  SEGNO: siamo il PRIMO motore -> Elo positivo = vinciamo noi." -ForegroundColor DarkGray

# Aggiudicazione identica a quella usata sul rig Linux (match.sh), o i due risultati
# non sarebbero confrontabili: la resa e la patta cambiano i punteggi.
& $fc `
  -engine cmd="$Engine7" dir="$(Split-Path $Engine7)" name="Triumviratus_7.0_$Variant" `
  -engine cmd="$e60"     dir="$Dest"                  name="Triumviratus_6.0" `
  -each proto=uci tc=$TC option.Hash=$Hash option.Threads=1 `
  -openings file="$Book" format=epd order=random `
  -repeat -rounds ([int]($Games/2)) -games 2 `
  -concurrency $Concurrency -recover `
  -resign movecount=3 score=600 twosided=true `
  -draw movenumber=40 movecount=8 score=10 `
  -pgnout file="$pgn" `
  -ratinginterval 20 -report penta=true 2>&1 | Tee-Object -FilePath $log

# --- profondita' media dai PGN --------------------------------------------------
Step "Profondita' media (dai PGN)"
$depths = @{}
$white = $null; $black = $null; $i = 0
foreach ($line in [System.IO.File]::ReadLines($pgn)) {
  if     ($line.StartsWith("[White ")) { $white = ($line -split '"')[1] }
  elseif ($line.StartsWith("[Black ")) { $black = ($line -split '"')[1]; $i = 0 }
  elseif ($line -and -not $line.StartsWith("[")) {
    foreach ($m in [regex]::Matches($line, '\{[^/}]*/(\d+)\s')) {
      $who = if ($i % 2 -eq 0) { $white } else { $black }
      if (-not $depths.ContainsKey($who)) { $depths[$who] = @(0,0) }
      $depths[$who][0] += [int]$m.Groups[1].Value
      $depths[$who][1] += 1
      $i++
    }
  }
}
foreach ($k in $depths.Keys) {
  $s = $depths[$k]
  if ($s[1] -gt 0) { Write-Host ("  {0,-32} {1:N2} ply   ({2} mosse)" -f $k, ($s[0]/$s[1]), $s[1]) -ForegroundColor Green }
}
Write-Host "`n  PGN: $pgn" -ForegroundColor DarkGray
Write-Host "  log: $log" -ForegroundColor DarkGray

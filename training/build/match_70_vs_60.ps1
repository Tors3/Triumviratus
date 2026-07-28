# =============================================================================
#  Gate 7.0 (legio-septima, fase 1 in corso) vs 6.0 (rubicon-alea-v3, release).
#  Entrambi PGO + AVX512: e' il primo confronto APPAIATO fra i due motori.
#
#  I numeri precedenti (-64,97 il 28/07) NON sono confrontabili: giravano con un
#  binario 7.0 di fatto AVX2, perche' il .vcxproj non definisce USE_AVX512.
#
#  USO:  .\match_70_vs_60.ps1
#        .\match_70_vs_60.ps1 -Concurrency 10 -TC "20+0.2" -Rounds 1000
# =============================================================================
param([int]$Concurrency = 14,
      [string]$TC = "15+0.15",
      [int]$Rounds = 700,
      [int]$Hash = 64)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

$e70    = "$root\Triumviratus_7\x64\Release\Triumviratus_7.0_avx512.exe"
$d70    = Split-Path $e70
$e60    = "$root\_release\Triumviratus_6.0\Triumviratus_6.0_avx512.exe"
$d60    = Split-Path $e60
$fc     = "$root\Fastechess_For_SPSA\fastchess.exe"
$book   = "$root\OpeningBooks\UHO_4060_v4\UHO_4060_v4.epd"
$outLog = "$root\_match_locale\gate_70_vs_60.log"
$outPgn = "$root\_match_locale\gate_70_vs_60.pgn"

foreach ($p in @($e70, $e60, $fc, $book)) {
    if (-not (Test-Path $p)) { throw "manca: $p" }
}
if (-not (Test-Path "$d70\nn-legio-septima.nnue"))    { throw "rete 7.0 mancante accanto all'exe: $d70" }
if (-not (Test-Path "$d60\nn-rubicon-alea-v3.nnue"))  { throw "rete 6.0 mancante accanto all'exe: $d60" }
New-Item -ItemType Directory -Force -Path "$root\_match_locale" | Out-Null

# Verifica che entrambi carichino la rete PRIMA di bruciare ore di partite:
# un motore che parte senza rete gioca comunque, e il risultato sarebbe spazzatura.
foreach ($pair in @(@($e70,"7.0"), @($e60,"6.0"))) {
    $out = "uci`nisready`nposition startpos`neval`nquit" | & $pair[0] 2>&1 | Out-String
    if ($out -match "(?m)^eval\s+(-?\d+)") {
        Write-Host ("  {0}: eval {1}" -f $pair[1], $Matches[1]) -ForegroundColor DarkGray
    } else {
        throw "$($pair[1]) non risponde a 'eval': rete non caricata?"
    }
}

Write-Host "`nGate 7.0-avx512-PGO vs 6.0-avx512-PGO  |  $TC  |  conc $Concurrency  |  $($Rounds*2) partite" -ForegroundColor Cyan
Write-Host "log: $outLog`n" -ForegroundColor DarkGray

& $fc `
  -engine cmd=$e70 dir=$d70 name=7.0-legio-ep111 `
  -engine cmd=$e60 dir=$d60 name=6.0-alea-v3 `
  -each tc=$TC option.Hash=$Hash option.Threads=1 proto=uci `
  -openings file=$book format=epd order=random `
  -rounds $Rounds -games 2 -repeat -concurrency $Concurrency `
  -pgnout file=$outPgn -ratinginterval 50 2>&1 | Tee-Object -FilePath $outLog

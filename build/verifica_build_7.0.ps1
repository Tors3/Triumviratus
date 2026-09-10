# =============================================================================
#  verifica_build_7.0.ps1 - le build 7.0 di questa cartella sono quelle giuste?
#
#  Mettilo accanto agli .exe, oppure lascialo nella radice del bootstrap: cerca
#  anche in _release\Triumviratus_7.0\. Poi lancialo:
#     powershell -ExecutionPolicy Bypass -File .\verifica_build_7.0.ps1
#  Opzioni:
#     -Exe C:\...\Triumviratus_7.0_avx2.exe   solo quegli eseguibili
#     -Dir C:\...\cartella                    cerca li' invece che qui
#     -NoBench                                salta il bench (piu' veloce)
#     -NoPause                                non aspetta Invio alla fine
#
#  Per ogni eseguibile controlla due cose:
#   1. BENCH = 240503. Tutta la matrice deve cercare lo stesso albero.
#   2. QUATTRO POSIZIONI a profondita' fissa, ognuna in un processo NUOVO: dentro la
#      stessa sessione `ucinewgame` non azzera tutto e i nodi cambiano. Nodi e mossa
#      devono tornare al numero, e il punteggio deve essere quello della scala 449
#      (ricalibrata il 10/09/2026). Nodi giusti ma punteggio vecchio = build con la
#      scala 392: il bootstrap non ha preso il commit 4759b7a.
#  Codice di uscita: 0 tutto ok, 1 almeno una build sbagliata, 2 alcune build non
#  eseguibili su questa CPU (le altre ok).
#
#  File solo ASCII apposta: Windows PowerShell 5.1 legge un .ps1 senza BOM come
#  ANSI, e un carattere accentato basta a rompere il parsing.
# =============================================================================
param(
  [string[]]$Exe = @(),
  [string]$Dir = "",
  [switch]$NoBench,
  [switch]$NoPause,
  [int]$TimeoutSec = 180
)
$ErrorActionPreference = "Stop"
$ExpectedBench = "240503"
$STATUS_ILLEGAL_INSTRUCTION = -1073741795   # 0xC000001D: la CPU non ha quella ISA
$NOT_RUNNABLE = "NON ESEGUIBILE SU QUESTA CPU"

# Misurati il 10/09/2026 sul rig: Threads 1, Hash 64, processo nuovo per posizione.
# Nodi e mossa sono gli stessi con la scala vecchia e con la nuova: cambia solo il numero.
$Cases = @(
  @{ Name = "startpos";     Depth = 18; Nodes = 512199;  Move = "g1f3"; New = 19;  Old = 21;
     Pos = "position startpos" },
  @{ Name = "patta KvK";    Depth = 18; Nodes = 17583;   Move = "d3e4"; New = 0;   Old = 0;
     Pos = "position fen 8/8/4k3/8/8/3K4/8/8 w - - 0 1" },
  @{ Name = "patta TvT";    Depth = 18; Nodes = 1227768; Move = "h1h5"; New = -3;  Old = -4;
     Pos = "position fen 8/3k4/8/8/8/8/r7/3K3R w - - 0 1" },
  @{ Name = "cavallo in +"; Depth = 16; Nodes = 640839;  Move = "e2e4"; New = 499; Old = 571;
     Pos = "position fen r1bqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1" }
)

# Una variante che la CPU non sa eseguire muore di istruzione illegale, e Windows puo'
# aprire la finestra "ha smesso di funzionare" e tenere fermo lo script finche' non la
# chiudi. SetErrorMode la sopprime, e i processi figli ereditano l'impostazione.
try {
  if (-not ("Triumv.ErrMode" -as [type])) {
    Add-Type -Namespace Triumv -Name ErrMode -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode);'
  }
  [void][Triumv.ErrMode]::SetErrorMode(3)   # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX
} catch { }

function Start-Engine([string]$path) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $path
  $psi.WorkingDirectory = Split-Path -Parent $path   # la rete sta accanto all'exe
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.RedirectStandardInput = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $p = [System.Diagnostics.Process]::Start($psi)
  $p.StandardInput.NewLine = "`n"
  $p.StandardInput.AutoFlush = $true
  # stderr svuotato in background: se il suo buffer si riempisse il motore si fermerebbe.
  [void]$p.StandardError.ReadToEndAsync()
  return $p
}

function Send-Lines($p, [string[]]$lines) {
  # Un processo gia' morto (istruzione illegale all'avvio) chiude la pipe: scriverci
  # lancia un'eccezione, che qui non deve fermare lo script.
  try { foreach ($l in $lines) { $p.StandardInput.WriteLine($l) } } catch { }
}

function Stop-Engine($p) {
  try { if (-not $p.HasExited) { $p.StandardInput.WriteLine("quit") } } catch { }
  if (-not $p.WaitForExit(10000)) {
    try { $p.Kill() } catch { }
    [void]$p.WaitForExit(5000)
  }
}

function Invoke-Bench([string]$path) {
  $p = Start-Engine $path
  Send-Lines $p @("uci", "isready", "bench", "quit")
  $t = $p.StandardOutput.ReadToEndAsync()
  if (-not $t.Wait($TimeoutSec * 1000)) {
    Stop-Engine $p
    return @{ Nodes = $null; Err = "timeout ${TimeoutSec}s"; Id = $null }
  }
  [void]$p.WaitForExit(10000)
  $out = $t.Result
  $r = @{ Nodes = $null; Err = $null; Id = $null }
  if ($out -match 'Nodes searched\s*:\s*(\d+)') { $r.Nodes = $Matches[1] }
  if ($out -match '(?m)^id name (.+)$')         { $r.Id = $Matches[1].Trim() }
  if (-not $r.Nodes) {
    if ($p.HasExited -and $p.ExitCode -eq $STATUS_ILLEGAL_INSTRUCTION) { $r.Err = "ISA" }
    else { $r.Err = "nessun bench in uscita" }
  }
  return $r
}

function Invoke-Case([string]$path, $case) {
  $r = @{ Depth = $null; Kind = $null; Score = $null; Nodes = $null; Move = $null; Id = $null; Err = $null }
  $p = Start-Engine $path
  Send-Lines $p @("uci", "setoption name Threads value 1", "setoption name Hash value 64",
                  "isready", "ucinewgame", $case.Pos, "go depth $($case.Depth)")
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ($true) {
    $left = [int]($deadline - (Get-Date)).TotalMilliseconds
    if ($left -le 0) { $r.Err = "timeout ${TimeoutSec}s"; break }
    $t = $p.StandardOutput.ReadLineAsync()
    if (-not $t.Wait($left)) { $r.Err = "timeout ${TimeoutSec}s"; break }
    $line = $t.Result
    if ($null -eq $line) { $r.Err = "il processo e' terminato"; break }
    if ($line -match '^id name (.+)') { $r.Id = $Matches[1].Trim() }
    # L'ultima riga "info depth" con un punteggio prima di bestmove e' quella finale.
    if ($line.StartsWith("info depth") -and $line.Contains(" score ")) {
      if ($line -match '\bdepth (\d+)')             { $r.Depth = [int]$Matches[1] }
      if ($line -match '\bscore (cp|mate) (-?\d+)') { $r.Kind = $Matches[1]; $r.Score = [int]$Matches[2] }
      if ($line -match '\bnodes (\d+)')             { $r.Nodes = [long]$Matches[1] }
    }
    if ($line -match '^bestmove (\S+)') { $r.Move = $Matches[1]; break }
  }
  Stop-Engine $p
  if ($r.Err -and $p.HasExited -and $p.ExitCode -eq $STATUS_ILLEGAL_INSTRUCTION) { $r.Err = "ISA" }
  return $r
}

# --- quali eseguibili --------------------------------------------------------
if ($Exe.Count -eq 0) {
  $dirs = if ($Dir) { @($Dir) } else { @($PSScriptRoot, (Join-Path $PSScriptRoot "_release\Triumviratus_7.0")) }
  foreach ($d in $dirs) {
    if (-not (Test-Path $d)) { continue }
    $Exe = @(Get-ChildItem -Path $d -Filter "Triumviratus_7.0_*.exe" -File | Sort-Object Name | ForEach-Object { $_.FullName })
    if ($Exe.Count -gt 0) { break }
  }
}
if ($Exe.Count -eq 0) {
  throw "Nessun Triumviratus_7.0_*.exe in $PSScriptRoot ne' in _release\Triumviratus_7.0\. Metti lo script accanto agli .exe, oppure usa -Dir o -Exe."
}
$Exe = @($Exe | ForEach-Object { (Resolve-Path $_).Path })

$cpu = "?"
try { $cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim() } catch { }
Write-Host ""
Write-Host "Verifica build Triumviratus 7.0 - CPU: $cpu" -ForegroundColor Cyan
Write-Host "bench atteso $ExpectedBench; punteggi sulla scala 449 (Threads 1, Hash 64, processo nuovo per posizione)"

# --- verifica -----------------------------------------------------------------
$summary = @()
foreach ($path in $Exe) {
  $leaf = Split-Path -Leaf $path
  Write-Host ""
  Write-Host "==== $leaf ====" -ForegroundColor Cyan
  $verdict = "OK"
  $idName = ""

  if (-not $NoBench) {
    $b = Invoke-Bench $path
    if ($b.Id) { $idName = $b.Id }
    if ($b.Err -eq "ISA") {
      $verdict = $NOT_RUNNABLE
    } elseif (-not $b.Nodes) {
      $verdict = "ERRORE"
      Write-Host "  bench         ERRORE: $($b.Err)" -ForegroundColor Red
    } elseif ($b.Nodes -ne $ExpectedBench) {
      $verdict = "BENCH ERRATO"
      Write-Host "  bench         $($b.Nodes)   atteso $ExpectedBench" -ForegroundColor Red
    } else {
      Write-Host "  bench         $($b.Nodes)   ok" -ForegroundColor Green
    }
  }
  if ($verdict -eq $NOT_RUNNABLE) {
    Write-Host "  la CPU non ha le istruzioni di questa variante: saltata" -ForegroundColor Yellow
    $summary += [pscustomobject]@{ Build = $leaf; Esito = $verdict; Versione = $idName }
    continue
  }

  foreach ($c in $Cases) {
    $r = Invoke-Case $path $c
    if (-not $idName -and $r.Id) { $idName = $r.Id }
    $got = if ($null -ne $r.Score) { "$($r.Kind) $($r.Score)" } else { "-" }
    if ($r.Err -eq "ISA") {
      $v = $NOT_RUNNABLE; $col = "Yellow"
    } elseif ($r.Err -or -not $r.Move) {
      $v = "ERRORE: $($r.Err)"; $col = "Red"
    } elseif ($r.Nodes -ne $c.Nodes -or $r.Move -ne $c.Move -or $r.Kind -ne "cp") {
      $v = "ALBERO DIVERSO (attesi $($c.Nodes) nodi, $($c.Move))"; $col = "Red"
    } elseif ($r.Score -eq $c.New) {
      $v = "ok"; $col = "Green"
    } elseif ($r.Score -eq $c.Old) {
      $v = "SCALA VECCHIA 392 (atteso cp $($c.New))"; $col = "Red"
    } else {
      $v = "PUNTEGGIO INATTESO (atteso cp $($c.New))"; $col = "Red"
    }
    Write-Host ("  {0,-13} d{1,-3} {2,-8} nodi {3,-8} {4,-5} {5}" -f $c.Name, $r.Depth, $got, $r.Nodes, $r.Move, $v) -ForegroundColor $col
    if ($v -ne "ok" -and $verdict -eq "OK") { $verdict = ($v -split ' \(')[0] }
  }
  $summary += [pscustomobject]@{ Build = $leaf; Esito = $verdict; Versione = $idName }
}

# --- riepilogo ------------------------------------------------------------------
Write-Host ""
Write-Host "==== RIEPILOGO ====" -ForegroundColor Cyan
$summary | Format-Table -AutoSize | Out-String | Write-Host
$bad  = @($summary | Where-Object { $_.Esito -ne "OK" -and $_.Esito -ne $NOT_RUNNABLE })
$skip = @($summary | Where-Object { $_.Esito -eq $NOT_RUNNABLE })
if ($bad.Count -gt 0) {
  Write-Host "$($bad.Count) build su $($summary.Count) NON sono a posto: non rilasciarle." -ForegroundColor Red
  $code = 1
} elseif ($skip.Count -gt 0) {
  Write-Host "Ok le build eseguibili; $($skip.Count) non verificabili su questa CPU: provale su una che le regge." -ForegroundColor Yellow
  $code = 2
} else {
  Write-Host "TUTTO OK: $($summary.Count) build, bench $ExpectedBench, stesso albero, scala 449." -ForegroundColor Green
  $code = 0
}
if (-not $NoPause) { [void](Read-Host "Invio per chiudere") }
exit $code

# =============================================================================
#  Build PGO (Profile-Guided Optimization) per Triumviratus, MSVC.
#  3 fasi:  1) STRUMENTA (ReleasePGI)  ->  2) ALLENA (pgo_train.py)  ->  3) OTTIMIZZA (ReleasePGO)
#  Produce x64\Release\Triumviratus_pgo.exe ottimizzato sul profilo reale.
#
#  USO:   .\build_pgo.ps1                  # profilo MAX: 200 pos x spettro movetime
#         .\build_pgo.ps1 -Positions 120    # profilo piu' leggero/veloce
#         .\build_pgo.ps1 -Movetime 150     # forza un singolo movetime
#         .\build_pgo.ps1 -Workers 8        # processi paralleli (default: 8)
#         .\build_pgo.ps1 -SkipToLink       # salta a fase 3 (obj+pgd gia' pronti)
#
#  NB: la config Release|x64 (Triumviratus_3.4.exe) non viene mai toccata.
#      Le config ReleasePGI e ReleasePGO sono dedicate al PGO.
#      Il training usa posizioni reali dal libro UHO a MOVETIME (non depth
#      fisso): profilo rappresentativo del gioco a tempo, niente overfit bench.
#
#  LAYOUT (dal vcxproj):
#      OutDir  -> x64\Release\          (.pgd scritto qui da PGInstrument)
#      IntDir  -> x64\ReleasePGI\       (oggetti intermedi)
#      .pgd    -> x64\Release\Triumviratus_pgo.pgd
#      .pgc    -> x64\Release\Triumviratus_pgo!N.pgc  (runtime, stesso OutDir)
# =============================================================================
param([int]$Movetime = 0, [int]$Positions = 200, [int]$Workers = 8, [switch]$SkipToLink)
$ErrorActionPreference = "Stop"

$root    = $PSScriptRoot   # repo-root (lo script vive qui) -> niente path assoluti
$proj    = "$root\Triumviratus_3.0\Triumviratus_3.0.vcxproj"

# OutDir condiviso da ReleasePGI e ReleasePGO (come da vcxproj)
$outDir  = "$root\Triumviratus_3.0\x64\Release"

$target  = "Triumviratus_pgo"
$exe     = "$outDir\$target.exe"
$pgdPath = "$outDir\$target.pgd"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot  = if (Test-Path $vswhere) { (& $vswhere -latest -property installationPath) } else { "" }
if (-not $vsRoot) { $vsRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community" }
$msbuild = "$vsRoot\MSBuild\Current\Bin\MSBuild.exe"

$commonPGI = @("-p:Configuration=ReleasePGI", "-p:Platform=x64", "-m", "-nologo", "-v:minimal")
# (ReleasePGO via MSBuild non viene usata: la fase 3 linka direttamente con link.exe)

function Step($n, $msg) { Write-Host "`n==== FASE $n : $msg ====" -ForegroundColor Cyan }

if (-not $SkipToLink) {

    # --- 1) STRUMENTA ------------------------------------------------------------
    Step 1 "Build strumentata (ReleasePGI -> /LTCG:PGInstrument)"
    
    & $msbuild $proj @commonPGI "-t:Rebuild"
    if ($LASTEXITCODE -ne 0) { throw "Build strumentata fallita." }
    
    if (-not (Test-Path $pgdPath)) {
        throw "Manca il .pgd in ${pgdPath}: la strumentazione non e' partita correttamente."
    }
    Write-Host "  -> exe strumentato : ${exe}" -ForegroundColor Green
    Write-Host "  -> .pgd             : ${pgdPath}" -ForegroundColor Green
    
    # pgort*.dll serve a runtime fuori dal Developer PowerShell
    $pgort = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Recurse -Filter "pgort*.dll" -ErrorAction SilentlyContinue |
             Where-Object { $_.FullName -match "Hostx64\\x64" } | Select-Object -First 1
    if ($pgort) {
        Copy-Item $pgort.FullName $outDir -Force
        Write-Host "  -> copiato $($pgort.Name) in ${outDir}" -ForegroundColor Green
    } else {
        Write-Warning "pgort*.dll non trovato: se il training non parte apri un 'Developer PowerShell for VS 2022' e rilancia."
    }
    
    # Pulisci .pgc di run precedenti (evita contaminazione del profilo)
    $stale = Get-ChildItem "$outDir\$target!*.pgc" -ErrorAction SilentlyContinue
    if ($stale) {
        $stale | Remove-Item -Force
        Write-Host "  -> rimossi $($stale.Count) .pgc residui da run precedenti" -ForegroundColor Yellow
    }
    
    # --- 2) ALLENA ---------------------------------------------------------------
    Step 2 "Training (genera .pgc) - $Positions posizioni, $Workers workers paralleli"
    
    $book = "$root\OpeningBooks\uho_2024\UHO_2024_+085_+094\UHO_2024_8mvs_big_+080_+099.epd"
    if (-not (Test-Path $book)) {
        throw "Libro EPD non trovato: ${book}"
    }
    
    # I .pgc vengono scritti nella CWD dell'exe strumentato -> deve essere $outDir
    # (stesso posto del .pgd, come vuole pgosweep/linker)
    Push-Location $outDir
    try {
        python "$root\pgo_train.py" $exe $Movetime $book $Positions --workers $Workers
        if ($LASTEXITCODE -ne 0) { throw "pgo_train.py ha restituito codice ${LASTEXITCODE}." }
    } finally {
        Pop-Location
    }
    
    $pgcFiles = @(Get-ChildItem "$outDir\$target!*.pgc" -ErrorAction SilentlyContinue)
    if ($pgcFiles.Count -eq 0) {
        throw "Nessun .pgc generato in ${outDir}.`nCause probabili: pgort*.dll mancante, exe che crasha, PATH non include $outDir."
    }
    Write-Host "  -> $($pgcFiles.Count) file .pgc generati in ${outDir}" -ForegroundColor Green
    
    # Consolida tutti i .pgc in un unico profilo con pgomgr.
    # Con N workers in parallelo ci saranno sempre N .pgc -> merge sempre eseguito.
    $pgomgr = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Recurse -Filter "pgomgr.exe" -ErrorAction SilentlyContinue |
              Where-Object { $_.FullName -match "Hostx64\\x64" } | Select-Object -First 1
    if ($pgomgr) {
        Write-Host "  -> pgomgr: merge di $($pgcFiles.Count) .pgc in $target.pgd ..." -ForegroundColor DarkGray
        & $pgomgr.FullName /merge "$outDir\$target.pgd"
        if ($LASTEXITCODE -ne 0) { Write-Warning "pgomgr ha restituito $LASTEXITCODE (non bloccante, il linker ritenta)." }
    } else {
        Write-Warning "pgomgr.exe non trovato: il linker leggera' i .pgc direttamente (piu' lento ma funziona)."
    }

} # end -not SkipToLink

# --- 3) OTTIMIZZA ------------------------------------------------------------
Step 3 "Build ottimizzata sul profilo (link.exe diretto -> /LTCG:PGOPTIMIZE)"

# MSBuild con ReleasePGO ricompila gli .obj perche' li vede out-of-date rispetto
# alla config, producendo nuovi .obj senza /GL -> LNK1263.
# La fase PGO non deve ricompilare nulla: si linka direttamente con gli .obj
# strumentati esistenti in IntDir usando link.exe con /LTCG:PGOPTIMIZE.
#
# CRITICO: link.exe DEVE essere della stessa versione del cl.exe che ha prodotto
# gli .obj (C1900 = mismatch P1/P2 se si usano versioni diverse installate).
# Ricaviamo link.exe dalla stessa cartella di cl.exe usato da MSBuild.

$intDir  = "$root\Triumviratus_3.0\x64\ReleasePGI"

# cl.exe piu' recente sotto Hostx64\x64 = quello scelto da MSBuild con v143
$clExe = Get-ChildItem "$vsRoot\VC\Tools\MSVC" -Recurse -Filter "cl.exe" -ErrorAction SilentlyContinue |
         Where-Object { $_.FullName -match "Hostx64\\x64" } |
         Sort-Object { $_.FullName } -Descending | Select-Object -First 1
if (-not $clExe) { throw "cl.exe non trovato sotto $vsRoot\VC\Tools\MSVC" }

# link.exe e' nella stessa cartella di cl.exe -> stessa versione garantita
$toolDir = $clExe.DirectoryName
$linkExe = Join-Path $toolDir "link.exe"
if (-not (Test-Path $linkExe)) { throw "link.exe non trovato in $toolDir" }
Write-Host "  -> toolchain : $toolDir" -ForegroundColor DarkGray
Write-Host "  -> link.exe  : $linkExe" -ForegroundColor DarkGray

# Raccogli tutti gli .obj prodotti dalla fase PGI (root + sottocartelle)
$objs = @(Get-ChildItem "$intDir\*.obj", "$intDir\fathom\*.obj", "$intDir\sfnnue\*.obj" `
          -ErrorAction SilentlyContinue | Select-Object -ExpandProperty FullName)
if ($objs.Count -eq 0) { throw "Nessun .obj trovato in ${intDir}: eseguire prima la fase PGI." }
Write-Host "  -> $($objs.Count) .obj trovati in ${intDir}" -ForegroundColor DarkGray

# Librerie di sistema necessarie (stessa lista del vcxproj Release)
$sysLibs = @("kernel32.lib","user32.lib","gdi32.lib","winspool.lib","comdlg32.lib",
             "advapi32.lib","shell32.lib","ole32.lib","oleaut32.lib","uuid.lib",
             "odbc32.lib","odbccp32.lib")

# link.exe invocato direttamente non eredita LIB dal Developer prompt.
# Ricaviamo i path di Windows SDK (um\x64) e MSVC libs dalla toolchain trovata.
$msvcLibDir = Join-Path (Split-Path (Split-Path (Split-Path $toolDir))) "lib\x64"

# Windows SDK: prende la versione piu' alta installata
$sdkRoot = "C:\Program Files (x86)\Windows Kits\10\Lib"
$sdkVer  = Get-ChildItem $sdkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkVer) { throw "Windows SDK non trovato in $sdkRoot" }
$sdkUmDir  = Join-Path $sdkVer.FullName "um\x64"
$sdkUcrtDir= Join-Path $sdkVer.FullName "ucrt\x64"
Write-Host "  -> MSVC libs : $msvcLibDir" -ForegroundColor DarkGray
Write-Host "  -> SDK um    : $sdkUmDir"   -ForegroundColor DarkGray
Write-Host "  -> SDK ucrt  : $sdkUcrtDir" -ForegroundColor DarkGray

# Argomenti linker
$linkArgs = $objs + $sysLibs + @(
    "/OUT:${exe}",
    "/LTCG:PGOPTIMIZE",
    "/USEPROFILE:PGD=${pgdPath}",
    "/SUBSYSTEM:CONSOLE",
    "/MACHINE:X64",
    "/NOLOGO",
    "/LIBPATH:$msvcLibDir",
    "/LIBPATH:$sdkUmDir",
    "/LIBPATH:$sdkUcrtDir"
)

& $linkExe @linkArgs
if ($LASTEXITCODE -ne 0) { throw "Link PGOptimize fallito (exit $LASTEXITCODE)." }
Write-Host "  -> ${exe} rilinkata con PGO." -ForegroundColor Green

# Pulizia profilo (tieni solo l'exe finale)
Remove-Item "$outDir\$target!*.pgc", "$outDir\$target.pgd" -ErrorAction SilentlyContinue
Write-Host "  -> file di profilo (.pgd, .pgc) rimossi." -ForegroundColor DarkGray

Write-Host "`n==== FATTO ====" -ForegroundColor Cyan
Write-Host "Exe PGO : ${exe}"
Write-Host "Confronta NPS vs il main (no-PGO):"
Write-Host "  python tools\nps_compare.py ${root}\x64\Release\Triumviratus_3.4.exe ${exe}"

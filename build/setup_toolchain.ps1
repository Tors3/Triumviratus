# =============================================================================
#  setup_toolchain.ps1 — rimette in piedi la catena di compilazione dopo un reset.
#
#  USO (PowerShell COME AMMINISTRATORE):
#      Set-ExecutionPolicy -Scope Process Bypass -Force
#      .\setup_toolchain.ps1
#
#  Installa solo cio' che serve alla catena di release (`release.ps1`, source\universal):
#    - Visual Studio 2022 Community + workload C++ desktop
#    - C++ Clang Compiler for Windows      -> clang-cl, llvm-profdata, clang_rt.profile
#    - MSBuild support for LLVM (clang-cl) -> PlatformToolset=ClangCL richiesto dal vcxproj
#    - LLVM 23 (clang-cl 23.1.4): il compilatore delle release, piu' veloce della clang di VS (vedi 4b)
#    - Python 3.11  (l'allenamento PGO, universal\pgo_train_det.py, usa SOLO la stdlib: nessun pacchetto pip)
#    - Git
#
#  ⚠️ I due componenti Clang NON sono inclusi nel workload C++ di default. Senza il
#     toolset MSBuild fallisce dicendo che ClangCL non esiste; senza il compilatore
#     non parte il merge dei profili PGO.
#  ⚠️ Il motore ha uno static_assert che RIFIUTA clang < 19. VS 17.14 porta la 19.1.5.
#  ⚠️ Questo script NON tocca reti, libri di apertura e artefatti: quelli si ripristinano
#     dal backup, non sono su GitHub.
# =============================================================================
$ErrorActionPreference = "Stop"

# --- 1. amministratore ------------------------------------------------------
$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Serve una PowerShell come AMMINISTRATORE (l'installer di VS lo pretende)." -ForegroundColor Red
    exit 1
}

# --- 2. winget --------------------------------------------------------------
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Host "winget non trovato. Su Windows 11 arriva con 'Installazione app'." -ForegroundColor Red
    Write-Host "Aprire Microsoft Store -> 'Programma di installazione app' -> Aggiorna, poi rilanciare." -ForegroundColor Yellow
    exit 1
}

function Step($m) { Write-Host "`n==== $m ====" -ForegroundColor Cyan }

# --- 3. Visual Studio + i due componenti Clang ------------------------------
# Gli ID dei componenti sono quelli ufficiali dell'installer VS; --override sostituisce
# INTERAMENTE gli argomenti di winget, quindi --quiet/--wait/--norestart vanno ripetuti qui.
Step "Visual Studio 2022 Community (+ workload C++ e componenti Clang)"
$vsArgs = @(
    "--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"
    "--add Microsoft.VisualStudio.Component.VC.Llvm.Clang"
    "--add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset"
    "--quiet --wait --norestart"
) -join " "
winget install --id Microsoft.VisualStudio.2022.Community `
    --accept-package-agreements --accept-source-agreements `
    --override $vsArgs

# --- 4. Python e Git --------------------------------------------------------
Step "Python 3.11"
winget install --id Python.Python.3.11 --accept-package-agreements --accept-source-agreements

Step "Git"
winget install --id Git.Git --accept-package-agreements --accept-source-agreements

# --- 4b. LLVM 23 --------------------------------------------------------------
# The release compiler (03/11/2026): clang 23 (LLVM 23.1.4) against the clang 19.1.5 of Visual Studio 17.14, same
# source and PGO, xperf 02/11/2026: cycles per node -0.65% middlegame / -1.31% endgames on a Xeon, level on a Zen 4.
# source\universal\ubuild_lib.ps1 (Find-Toolchain) takes the newest complete LLVM among %ProgramFiles%\LLVM and
# Visual Studio; without this step the builds use the Visual Studio one. If winget does not offer 23.1.4, the latest.
Step "LLVM 23 (clang-cl, lld-link, llvm-profdata)"
winget install --id LLVM.LLVM --version 23.1.4 --accept-package-agreements --accept-source-agreements
if ($LASTEXITCODE -ne 0) { winget install --id LLVM.LLVM --accept-package-agreements --accept-source-agreements }

# --- 5. verifica ------------------------------------------------------------
# Non basta che l'installazione "sia andata": si controlla che i quattro binari su cui
# poggia la pipeline esistano davvero e che clang sia >= 19.
Step "VERIFICA"
$ok = $true
function Check($nome, $cond, $dettaglio) {
    if ($cond) { Write-Host ("  [ok]      {0,-22} {1}" -f $nome, $dettaglio) -ForegroundColor Green }
    else       { Write-Host ("  [MANCA]   {0,-22} {1}" -f $nome, $dettaglio) -ForegroundColor Red
                 $script:ok = $false }
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot  = if (Test-Path $vswhere) { & $vswhere -latest -property installationPath } else { "" }
Check "Visual Studio" ([bool]$vsRoot) $vsRoot

if ($vsRoot) {
    $msbuild  = "$vsRoot\MSBuild\Current\Bin\MSBuild.exe"
    $clangcl  = "$vsRoot\VC\Tools\Llvm\x64\bin\clang-cl.exe"
    $profdata = "$vsRoot\VC\Tools\Llvm\x64\bin\llvm-profdata.exe"
    $rtlib    = Resolve-Path "$vsRoot\VC\Tools\Llvm\x64\lib\clang\*\lib\windows\clang_rt.profile-x86_64.lib" -ErrorAction SilentlyContinue

    Check "MSBuild"       (Test-Path $msbuild)  $msbuild
    Check "llvm-profdata" (Test-Path $profdata) $profdata

    if (Test-Path $clangcl) {
        $ver = (& $clangcl --version | Select-String -Pattern '(\d+)\.\d+\.\d+').Matches.Groups[1].Value
        Check "clang-cl >= 19" ([int]$ver -ge 19) "versione $ver  ($clangcl)"
    } else { Check "clang-cl >= 19" $false "non trovato" }

    Check "clang_rt.profile" ([bool]$rtlib) $(if ($rtlib) { $rtlib.Path } else { "assente: la PGO non linka" })
}

# LLVM standalone (4b): not required (the Visual Studio clang is the fallback), but the release compiler.
$llvmCl = "$env:ProgramFiles\LLVM\bin\clang-cl.exe"
if (Test-Path $llvmCl) {
    $lv = (& $llvmCl --version | Select-String -Pattern '(\d+)\.\d+\.\d+').Matches.Groups[1].Value
    if ([int]$lv -ge 23) { Write-Host ("  [ok]      {0,-22} {1}" -f "LLVM >= 23", "versione $lv  ($llvmCl)") -ForegroundColor Green }
    else { Write-Host ("  [vecchia] {0,-22} {1}" -f "LLVM >= 23", "versione ${lv}: le build useranno la clang piu' recente") -ForegroundColor Yellow }
} else {
    Write-Host ("  [assente] {0,-22} {1}" -f "LLVM >= 23", "le build useranno la clang di Visual Studio (piu' lenta)") -ForegroundColor Yellow
}

# I comandi appena installati non sono nel PATH di QUESTA sessione: si cercano su disco.
$py  = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1 }
Check "Python" ([bool]$py) $(if ($py) { if ($py.Source) { $py.Source } else { $py.FullName } } else { "" })

$git = Get-Command git -ErrorAction SilentlyContinue
if (-not $git) { $git = Get-Item "$env:ProgramFiles\Git\cmd\git.exe" -ErrorAction SilentlyContinue }
Check "Git" ([bool]$git) $(if ($git) { if ($git.Source) { $git.Source } else { $git.FullName } } else { "" })

Write-Host ""
if ($ok) {
    Write-Host "Catena completa. Riapri una PowerShell NUOVA (per il PATH), poi:" -ForegroundColor Green
    Write-Host '  git clone https://github.com/<utente>/Triumviratus.git' -ForegroundColor Gray
    Write-Host "  ripristina dal backup: Networks_Triumviratus_7\, OpeningBooks\, _artifacts\, Tuning_SPSA\" -ForegroundColor Gray
    Write-Host '  ssh-keygen -t ed25519    # la vecchia chiave e'' compromessa' -ForegroundColor Gray
    Write-Host '  release build: build\release.ps1 (variants of this CPU, PGO, bench check; usage at the top of the script)' -ForegroundColor Gray
    Write-Host '  one-shot build: source\universal\build_universal.ps1 (expected bench in source\universal\release.json)' -ForegroundColor Gray
} else {
    Write-Host "Qualcosa manca: rilancia l'installer di VS e aggiungi a mano i componenti segnati sopra." -ForegroundColor Red
    Write-Host "  Componenti: VC.Llvm.Clang  e  VC.Llvm.ClangToolset" -ForegroundColor Yellow
}

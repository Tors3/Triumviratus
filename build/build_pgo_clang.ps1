# =============================================================================
#  Build clang-cl + ThinLTO + PGO (IR-based) per Triumviratus.
#  La leva NPS piu' grossa trovata (clang+LTO = +10% su MSVC; +PGO atteso ~+10% sulla
#  produzione MSVC+PGO). 4 fasi:
#    1) STRUMENTA  : clang-cl + ThinLTO + -fprofile-generate
#    2) ALLENA     : pgo_train.py su posizioni reali -> *.profraw (LLVM_PROFILE_FILE)
#    3) MERGE      : llvm-profdata merge -> clang.profdata
#    4) OTTIMIZZA  : clang-cl + ThinLTO + -fprofile-use -> Triumviratus_3.6.exe
#
#  USO:  .\build_pgo_clang.ps1                 # 200 pos, 8 workers
#        .\build_pgo_clang.ps1 -Positions 120 -Workers 8
#
#  ⚠️ Laptop-only (AVX512/Zen4). Per AWS/distribuzione serve un build MSVC-AVX2.
#  ⚠️ Misura NPS a laptop SCARICO (senno' rumore). Toolset: VS LLVM clang 19 + llvm-profdata.
#  Non tocca l'exe di produzione MSVC (Triumviratus_pgo.exe) ne' la Release MSVC.
# =============================================================================
param([int]$Movetime = 0, [int]$Positions = 200, [int]$Workers = 8)
$ErrorActionPreference = "Stop"

$root    = $PSScriptRoot                                # repo-root (lo script vive qui) -> niente path assoluti
$proj    = "$root\Triumviratus_3.0\Triumviratus_3.0.vcxproj"
$outDir  = "$root\Triumviratus_3.0\x64\Release"        # OutDir del vcxproj (inner)
$exe     = "$outDir\Triumviratus_3.4.exe"              # target Release
# VS install: dependency di SISTEMA (non del repo). Scoperto via vswhere, fallback al path standard.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot  = if (Test-Path $vswhere) { (& $vswhere -latest -property installationPath) } else { "" }
if (-not $vsRoot) { $vsRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community" }
$msbuild = "$vsRoot\MSBuild\Current\Bin\MSBuild.exe"
$profdata= "$vsRoot\VC\Tools\Llvm\x64\bin\llvm-profdata.exe"

# clang profiling runtime: lld-link cerca "clang_rt.profile.lib" (senza suffisso arch). La creiamo
# self-contained copiandola (rinominata) in <repo>\clang_rt_lib dal lib-clang di VS (versione auto-rilevata).
# Il vcxproj usa $(MSBuildProjectDirectory)\..\clang_rt_lib + $(ClangProfileLibDir) (passato sotto).
$clangLibDir = (Resolve-Path "$vsRoot\VC\Tools\Llvm\x64\lib\clang\*\lib\windows" -ErrorAction SilentlyContinue | Select-Object -Last 1).Path
$rtLib = "$root\clang_rt_lib"
New-Item -ItemType Directory -Force -Path $rtLib | Out-Null
if ($clangLibDir) {
    $srcLib = Join-Path $clangLibDir "clang_rt.profile-x86_64.lib"
    if (Test-Path $srcLib) { Copy-Item $srcLib "$rtLib\clang_rt.profile.lib" -Force }
}
$profDir = "$root\clang_pgo_profiles"
$merged  = "$profDir\clang.profdata"
$book    = "$root\OpeningBooks\uho_2024\UHO_2024_+085_+094\UHO_2024_8mvs_big_+080_+099.epd"
$finalInner = "$outDir\Triumviratus_3.6.exe"
$finalRepo  = "$root\x64\Release\Triumviratus_3.6.exe"

function Step($n,$m){ Write-Host "`n==== FASE $n : $m ====" -ForegroundColor Cyan }

if (-not (Test-Path $profdata)) { throw "llvm-profdata mancante: $profdata (installa 'C++ Clang tools for Windows' nel VS Installer)" }
if (-not (Test-Path $book))     { throw "Libro EPD non trovato: $book" }

if (Test-Path $profDir) { Remove-Item "$profDir\*" -Force -ErrorAction SilentlyContinue } else { New-Item -ItemType Directory -Force -Path $profDir | Out-Null }

$common = @("-p:Configuration=Release","-p:Platform=x64","-p:PlatformToolset=ClangCL","-m","-nologo","-v:minimal","-p:ClangProfileLibDir=$clangLibDir")

# --- 1) STRUMENTA ------------------------------------------------------------
Step 1 "Build strumentata (clang-cl + ThinLTO + -fprofile-generate)"
& $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-generate -DCLANG_PGO_GEN" "-t:Rebuild"
if ($LASTEXITCODE -ne 0) { throw "Build strumentata clang fallita." }
if (-not (Test-Path $exe)) { throw "Exe strumentato mancante: $exe" }

# --- 2) ALLENA ---------------------------------------------------------------
Step 2 "Training ($Positions posizioni, $Workers workers) -> .profraw"
# Il path di exit del motore NON fa scattare l'atexit di LLVM -> il quit-handler (con
# -DCLANG_PGO_GEN) chiama __llvm_profile_write_file() esplicitamente. %p = per-pid (ogni
# worker il suo profraw COMPLETO). NB: continuous mode (%c) dava file TRUNCATED -> non si usa.
$env:LLVM_PROFILE_FILE = "$profDir\prof_%p.profraw"
Push-Location $outDir
try {
    python "$root\pgo_train.py" $exe $Movetime $book $Positions --workers $Workers
    if ($LASTEXITCODE -ne 0) { throw "pgo_train.py exit $LASTEXITCODE" }
} finally {
    Pop-Location
    $env:LLVM_PROFILE_FILE = $null
}
$raws = @(Get-ChildItem "$profDir\*.profraw" -ErrorAction SilentlyContinue)
if ($raws.Count -eq 0) { throw "Nessun .profraw in $profDir (LLVM_PROFILE_FILE non ereditato dall'exe? exe crasha senza 'quit' pulito?)." }
Write-Host "  -> $($raws.Count) .profraw generati" -ForegroundColor Green

# --- 3) MERGE ----------------------------------------------------------------
Step 3 "llvm-profdata merge -> clang.profdata"
& $profdata merge -output="$merged" ($raws | Select-Object -ExpandProperty FullName)
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $merged)) { throw "llvm-profdata merge fallito." }
Write-Host "  -> $merged" -ForegroundColor Green

# --- 4) OTTIMIZZA ------------------------------------------------------------
Step 4 "Build ottimizzata (clang-cl + ThinLTO + -fprofile-use)"
& $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-use=$merged" "-t:Rebuild"
if ($LASTEXITCODE -ne 0) { throw "Build ottimizzata clang fallita." }
Copy-Item $exe $finalInner -Force
if (Test-Path (Split-Path $finalRepo)) { Copy-Item $exe $finalRepo -Force }

Write-Host "`n==== FATTO ====" -ForegroundColor Cyan
Write-Host "clang-PGO exe : $finalInner"
Write-Host "             + $finalRepo"
Write-Host "`nConfronto NPS (laptop SCARICO!):" -ForegroundColor Yellow
Write-Host "  .\tools\measure_nps.ps1 `"$finalRepo`" 8000 5"
Write-Host "  .\tools\measure_nps.ps1 `"$root\x64\Release\Triumviratus_pgo.exe`" 8000 5"
Write-Host "Atteso clang-PGO ~+10% su MSVC-PGO. Se conferma -> diventa il build di release (laptop)."

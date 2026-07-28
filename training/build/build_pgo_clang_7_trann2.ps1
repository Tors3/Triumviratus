# =============================================================================
#  Build RELEASE clang-cl + ThinLTO + PGO (IR-based) per TRIUMVIRATUS 7.0 TRANN2
#  (= SFNNv16 + blocco PassedPawns nostro). Rete: legio-septima, caricata a
#  runtime ACCANTO all'exe.
#
#  Adattato da build_pgo_clang_6_trann1.ps1. Differenze rispetto alla 6.0:
#
#   1) 🔴 Il .vcxproj della 7.0 NON definisce USE_AVX512/USE_VNNI/USE_AVX512ICL.
#      Il codice NNUE gatta i percorsi SIMD su `#if defined(USE_AVX512)`, quindi
#      senza quelle macro si compila AVX2 anche avendo i flag -mavx512*. Le
#      aggiungiamo qui, allineate a quel che fa `make avx512` nel Makefile.
#   2) La rete e' parametrica (-Net): la fase 1 e' ancora in corso e il
#      checkpoint da spedire non e' deciso.
#
#  4 fasi:  1) STRUMENTA (clang-cl + ThinLTO + -fprofile-generate)
#           2) ALLENA   (pgo_train.py su posizioni reali -> *.profraw)
#           3) MERGE    (llvm-profdata merge -> clang.profdata)
#           4) OTTIMIZZA(clang-cl + ThinLTO + -fprofile-use -> exe finale)
#
#  USO:  .\build_pgo_clang_7_trann2.ps1 -Arch avx512 -Release
#        .\build_pgo_clang_7_trann2.ps1 -Net "Networks_Triumviratus_7\legio-ep200.nnue"
#
#  Allena/misura a laptop SCARICO. Toolset: VS LLVM clang + llvm-profdata.
# =============================================================================
param([int]$Movetime = 0, [int]$Positions = 200, [int]$Workers = 8,
      [string]$Name = "Triumviratus_7.0",
      [string]$Net  = "Networks_Triumviratus_7\legio-ep111.nnue",
      [ValidateSet("both","avx512","avx2")][string]$Arch = "avx512",
      [switch]$Release)
$ErrorActionPreference = "Stop"
# -Release => -DTRIUMV_RELEASE: nasconde le opzioni UCI di tuning.
$reldef = if ($Release) { " -DTRIUMV_RELEASE" } else { "" }

$root    = $PSScriptRoot
$proj    = "$root\Triumviratus_7\Triumviratus_7.0.vcxproj"
if (-not (Test-Path $proj)) { throw "vcxproj 7.0 non trovato: $proj" }
$projDir = Split-Path $proj
$outDir  = "$projDir\x64\Release"
$exe     = "$outDir\Triumviratus_7.0.exe"
$netName = "nn-legio-septima.nnue"          # nome che il motore cerca (EvalFileDefaultName)
$netSrc  = if ([System.IO.Path]::IsPathRooted($Net)) { $Net } else { "$root\$Net" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsRoot  = if (Test-Path $vswhere) { (& $vswhere -latest -property installationPath) } else { "" }
if (-not $vsRoot) { $vsRoot = "C:\Program Files\Microsoft Visual Studio\2022\Community" }
$msbuild = "$vsRoot\MSBuild\Current\Bin\MSBuild.exe"
$profdata= "$vsRoot\VC\Tools\Llvm\x64\bin\llvm-profdata.exe"

$clangLibDir = (Resolve-Path "$vsRoot\VC\Tools\Llvm\x64\lib\clang\*\lib\windows" -ErrorAction SilentlyContinue | Select-Object -Last 1).Path
$rtLib = "$root\clang_rt_lib"
New-Item -ItemType Directory -Force -Path $rtLib | Out-Null
if ($clangLibDir) {
    $srcLib = Join-Path $clangLibDir "clang_rt.profile-x86_64.lib"
    if (Test-Path $srcLib) { Copy-Item $srcLib "$rtLib\clang_rt.profile.lib" -Force }
}
$profDir = "$root\clang_pgo_profiles"
$book    = "$root\OpeningBooks\uho_2024\UHO_2024_+085_+094\UHO_2024_8mvs_+085_+094.epd"
$trainer = "$root\pgo_train.py"

function Step($n,$m){ Write-Host "`n==== $m ====" -ForegroundColor Cyan }

if (-not (Test-Path $msbuild))  { throw "MSBuild mancante: $msbuild" }
if (-not (Test-Path $profdata)) { throw "llvm-profdata mancante: $profdata (VS Installer: 'C++ Clang tools for Windows')" }
if (-not (Test-Path $book))     { throw "Libro EPD non trovato: $book" }
if (-not (Test-Path $trainer))  { throw "pgo_train.py non trovato: $trainer" }
if (-not (Test-Path $netSrc))   { throw "Rete non trovata: $netSrc (passala con -Net)" }

Write-Host "rete: $netSrc" -ForegroundColor DarkGray

# Il motore carica la rete all'avvio (FATAL se manca) e il training PGO deve
# girare sulla rete VERA, non su una qualunque.
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Copy-Item $netSrc "$outDir\$netName" -Force

$common = @("-p:Configuration=Release","-p:Platform=x64","-p:PlatformToolset=ClangCL",
            "-m","-nologo","-v:minimal","-p:ClangProfileLibDir=$clangLibDir")

function Build-Variant([string]$tag) {
    $suffix = "_$tag"
    $merged = "$profDir\clang7trann2_$tag.profdata"
    Write-Host "`n########  VARIANTE 7.0 TRANN2 $tag  ->  $Name$suffix.exe  ########" -ForegroundColor Magenta

    # PGO e' gia' specifico per questa macchina (profilato QUI), quindi tune=native
    # e' la scelta coerente. Effetto misurato sulla 6.0: dentro il rumore, tenuto
    # perche' e' un puro hint di scheduling senza effetti su memoria o correttezza.
    $extra = " /clang:-mtune=native"
    if ($tag -eq "avx512") {
        # 🔴 Le MACRO servono: il codice gatta su #if defined(USE_AVX512). Senza,
        # i flag -m abilitano le istruzioni ma i percorsi restano quelli AVX2.
        # Stesso insieme di `make avx512` (Makefile:66-68).
        $extra += " -DUSE_AVX512 -DUSE_VNNI -DUSE_AVX512ICL"
        $extra += " /clang:-mavx512f /clang:-mavx512bw /clang:-mavx512dq /clang:-mavx512vl /clang:-mavx512vnni /clang:-mavx512vbmi /clang:-mavx512vbmi2 /clang:-mavx512bitalg /clang:-mbmi2"
    } elseif ($tag -eq "avx2") {
        # Il .vcxproj ha i -mavx512* cablati in AdditionalOptions: qui li spegniamo.
        $extra += " /clang:-mno-avx512f /clang:-mno-avx512bw /clang:-mno-avx512dq /clang:-mno-avx512vl /clang:-mno-avx512vnni"
    }

    if (Test-Path $profDir) { Remove-Item "$profDir\*.profraw" -Force -ErrorAction SilentlyContinue }
    else { New-Item -ItemType Directory -Force -Path $profDir | Out-Null }

    Step 1 "[$tag] FASE 1: build strumentata"
    & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-generate -DCLANG_PGO_GEN$extra$reldef /clang:-ffp-contract=off" "-t:Rebuild"
    if ($LASTEXITCODE -ne 0) { throw "[$tag] build strumentata fallita" }
    if (-not (Test-Path "$outDir\$netName")) { Copy-Item $netSrc "$outDir\$netName" -Force }

    Step 2 "[$tag] FASE 2: training ($Positions posizioni, $Workers worker)"
    $env:LLVM_PROFILE_FILE = "$profDir\prof_%p.profraw"
    Push-Location $outDir
    try {
        python $trainer $exe $Movetime $book $Positions --workers $Workers
        if ($LASTEXITCODE -ne 0) { throw "[$tag] pgo_train.py exit $LASTEXITCODE" }
    } finally { Pop-Location; $env:LLVM_PROFILE_FILE = $null }
    $raws = @(Get-ChildItem "$profDir\*.profraw" -ErrorAction SilentlyContinue)
    if ($raws.Count -eq 0) { throw "[$tag] nessun .profraw generato" }

    Step 3 "[$tag] FASE 3: merge profili ($($raws.Count) file)"
    & $profdata merge -output="$merged" ($raws | Select-Object -ExpandProperty FullName)
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $merged)) { throw "[$tag] llvm-profdata merge fallito" }

    Step 4 "[$tag] FASE 4: build ottimizzata (-fprofile-use)"
    & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-use=$merged$extra$reldef /clang:-ffp-contract=off" "-t:Rebuild"
    if ($LASTEXITCODE -ne 0) { throw "[$tag] build ottimizzata fallita" }

    if (-not (Test-Path "$outDir\$netName")) { Copy-Item $netSrc "$outDir\$netName" -Force }
    Copy-Item $exe "$outDir\$Name$suffix.exe" -Force
    Write-Host "  -> $Name$suffix.exe pronto" -ForegroundColor Green

    # Verifica che il binario carichi la rete e sappia cercare: se la rete non c'e'
    # il motore muore all'avvio, e un exe che non bencha non va spedito.
    Push-Location $outDir
    try {
        $out = "uci`nisready`nbench`nquit" | & ".\$Name$suffix.exe" 2>&1 | Out-String
        if ($out -match "Nodes searched\s*:\s*(\d+)") {
            Write-Host "  bench: $($Matches[1]) nodi" -ForegroundColor Green
        } else {
            Write-Warning "  [$tag] il bench non ha prodotto un conteggio nodi: controlla la rete"
        }
    } finally { Pop-Location }

    $junk = @("*.profraw","*.iobj","*.pdb","*.ilk","*.exp","pgort*.dll",
              "*.Build.CppClean.log","*.exe.recipe","vcpkg.applocal.log","*.FileListAbsolute.txt")
    foreach ($pat in $junk) { Remove-Item (Join-Path $outDir $pat) -Force -ErrorAction SilentlyContinue }
    Remove-Item "$profDir\*.profraw" -Force -ErrorAction SilentlyContinue
}

$variants = switch ($Arch) { "both" { @("avx512","avx2") } default { @($Arch) } }
foreach ($v in $variants) { Build-Variant $v }

Write-Host "`n==== 7.0 TRANN2 PGO PRONTA ====" -ForegroundColor Cyan
foreach ($v in $variants) { Write-Host "  $outDir\$Name`_$v.exe  (+ $netName accanto)" }

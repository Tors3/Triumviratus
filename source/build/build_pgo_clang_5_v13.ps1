# =============================================================================
#  Build RELEASE clang-cl + ThinLTO + PGO (IR-based) per TRIUMVIRATUS 5.0 SFNNv13.
#  Adattato da build_pgo_clang_5.ps1 (che era BULLET) al motore v13: la rete e'
#  ora nn-71d6d32cb962.nnue (SFNNv13, caricata a runtime ACCANTO all'exe). Niente
#  bullet -> niente PGO_BULLET_NET.
#
#  4 fasi:  1) STRUMENTA (clang-cl + ThinLTO + -fprofile-generate)
#           2) ALLENA   (pgo_train.py su posizioni reali -> *.profraw)
#           3) MERGE    (llvm-profdata merge -> clang.profdata)
#           4) OTTIMIZZA(clang-cl + ThinLTO + -fprofile-use -> exe finale)
#
#  USO:  .\build_pgo_clang_5_v13.ps1                 # Triumviratus_5.0_avx512.exe
#        .\build_pgo_clang_5_v13.ps1 -Positions 300  # piu' training
#  Allena/misura a laptop SCARICO. Toolset: VS LLVM clang + llvm-profdata.
# =============================================================================
param([int]$Movetime = 0, [int]$Positions = 200, [int]$Workers = 8,
      [string]$Name = "Triumviratus_5.0",
      [ValidateSet("both","avx512","avx2")][string]$Arch = "avx512",
      [switch]$Release)
$ErrorActionPreference = "Stop"
# -Release => -DTRIUMV_RELEASE: hides tuning UCI options (ships only Hash/Threads/Move Overhead/EvalFile/SyzygyPath).
$reldef = if ($Release) { " -DTRIUMV_RELEASE" } else { "" }

$root    = $PSScriptRoot
$proj    = "$root\Triumviratus_5\Triumviratus_5.0.vcxproj"
if (-not (Test-Path $proj)) { throw "vcxproj 5.0 non trovato: $proj" }
$projDir = Split-Path $proj
$outDir  = "$projDir\x64\Release"
$exe     = "$outDir\Triumviratus_5.0.exe"
$net5    = "nn-71d6d32cb962.nnue"                              # rete SFNNv13 (runtime, accanto all'exe)
$net5src = "$projDir\nnue\nn-71d6d32cb962.nnue"          # sorgente

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
$book    = "$root\OpeningBooks\uho_2024\UHO_2024_+085_+094\UHO_2024_8mvs_big_+080_+099.epd"
$trainer = "$root\pgo_train.py"

function Step($n,$m){ Write-Host "`n==== $m ====" -ForegroundColor Cyan }

if (-not (Test-Path $profdata)) { throw "llvm-profdata mancante: $profdata (VS Installer: 'C++ Clang tools for Windows')" }
if (-not (Test-Path $book))     { throw "Libro EPD non trovato: $book (serve per il training PGO)" }
if (-not (Test-Path $trainer))  { throw "pgo_train.py non trovato: $trainer" }

# La rete SFNNv13 deve stare accanto all'exe: il motore la carica all'avvio (FATAL se manca).
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
if (-not (Test-Path "$outDir\$net5")) {
    if (Test-Path $net5src) { Copy-Item $net5src "$outDir\$net5" -Force }
    else { throw "rete SFNNv13 non trovata: $net5src" }
}

$common = @("-p:Configuration=Release","-p:Platform=x64","-p:PlatformToolset=ClangCL","-m","-nologo","-v:minimal","-p:ClangProfileLibDir=$clangLibDir")

function Build-Variant([string]$tag) {
    $suffix = "_$tag"
    $merged = "$profDir\clang5v13_$tag.profdata"
    Write-Host "`n########  VARIANTE 5.0 v13 $tag  ->  $Name$suffix.exe  ########" -ForegroundColor Magenta

    $extra = ""
    if ($tag -eq "avx512") {
        # +VBMI/VBMI2/BITALG for the USE_AVX512ICL fast paths (compress_epi16/epi8,
        # permutexvar_epi8) — Zen4/Ice Lake+. clang gates intrinsics by -m flags (MSVC doesn't).
        $extra = " /clang:-mavx512f /clang:-mavx512bw /clang:-mavx512dq /clang:-mavx512vl /clang:-mavx512vnni /clang:-mavx512vbmi /clang:-mavx512vbmi2 /clang:-mavx512bitalg /clang:-mbmi2"
    } elseif ($tag -eq "avx2") {
        $extra = " /clang:-mno-avx512f /clang:-mno-avx512bw /clang:-mno-avx512dq /clang:-mno-avx512vl /clang:-mno-avx512cd /clang:-mno-avx512vnni"
    }

    $orig = Get-Content $proj -Raw
    try {
        if ($tag -eq "avx2") {
            # Strip every AVX512 token (incl. USE_AVX512ICL) wherever it sits; keep PEXT+AVX2.
            $xml = $orig -replace "USE_VNNI;", "" -replace "USE_AVX512ICL;", "" -replace "USE_AVX512;", ""
            $xml = $xml -replace "AdvancedVectorExtensions512", "AdvancedVectorExtensions2"
            if ($xml -match "USE_AVX512") { throw "[$tag] patch vcxproj fallita: USE_AVX512 ancora presente" }
            Set-Content $proj $xml -Encoding UTF8
        }
        if (Test-Path $profDir) { Remove-Item "$profDir\*.profraw" -Force -ErrorAction SilentlyContinue } else { New-Item -ItemType Directory -Force -Path $profDir | Out-Null }

        Step 1 "[$tag] FASE 1: build strumentata"
        & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-generate -DCLANG_PGO_GEN$extra$reldef /clang:-ffp-contract=off" "-t:Rebuild"
        if ($LASTEXITCODE -ne 0) { throw "[$tag] build strumentata fallita" }

        # la rete deve esserci anche dopo il Rebuild
        if (-not (Test-Path "$outDir\$net5") -and (Test-Path $net5src)) { Copy-Item $net5src "$outDir\$net5" -Force }

        Step 2 "[$tag] FASE 2: training ($Positions pos, $Workers workers)"
        $env:LLVM_PROFILE_FILE = "$profDir\prof_%p.profraw"
        Push-Location $outDir
        try {
            python $trainer $exe $Movetime $book $Positions --workers $Workers
            if ($LASTEXITCODE -ne 0) { throw "[$tag] pgo_train.py exit $LASTEXITCODE" }
        } finally { Pop-Location; $env:LLVM_PROFILE_FILE = $null }
        $raws = @(Get-ChildItem "$profDir\*.profraw" -ErrorAction SilentlyContinue)
        if ($raws.Count -eq 0) { throw "[$tag] nessun .profraw generato" }

        Step 3 "[$tag] FASE 3: merge profili"
        & $profdata merge -output="$merged" ($raws | Select-Object -ExpandProperty FullName)
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $merged)) { throw "[$tag] llvm-profdata merge fallito" }

        Step 4 "[$tag] FASE 4: build ottimizzata (-fprofile-use)"
        & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-use=$merged$extra$reldef /clang:-ffp-contract=off" "-t:Rebuild"
        if ($LASTEXITCODE -ne 0) { throw "[$tag] build ottimizzata fallita" }

        if (-not (Test-Path "$outDir\$net5") -and (Test-Path $net5src)) { Copy-Item $net5src "$outDir\$net5" -Force }
        Copy-Item $exe "$outDir\$Name$suffix.exe" -Force
        Write-Host "  -> $Name$suffix.exe pronto" -ForegroundColor Green
    } finally {
        Set-Content $proj $orig -Encoding UTF8       # vcxproj sempre ripristinato
    }

    $junk = @("*.profraw","*.iobj","*.pdb","*.ilk","*.exp","pgort*.dll",
              "*.Build.CppClean.log","*.exe.recipe","vcpkg.applocal.log","*.FileListAbsolute.txt")
    foreach ($pat in $junk) { Remove-Item (Join-Path $outDir $pat) -Force -ErrorAction SilentlyContinue }
    Remove-Item "$profDir\*.profraw" -Force -ErrorAction SilentlyContinue
}

$variants = switch ($Arch) { "both" { @("avx512","avx2") } default { @($Arch) } }
foreach ($v in $variants) { Build-Variant $v }

Write-Host "`n==== 5.0 v13 PGO PRONTA ====" -ForegroundColor Cyan
foreach ($v in $variants) { Write-Host "  $outDir\$Name`_$v.exe  (+ $net5 accanto)" }

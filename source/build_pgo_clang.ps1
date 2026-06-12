# =============================================================================
#  Build di RELEASE clang-cl + ThinLTO + PGO (IR-based) per Triumviratus 4.0+.
#  Produce DUE varianti distribuibili (entrambe PGO-ottimizzate):
#    - <Name>_avx512.exe : CPU moderne (AVX-512+VNNI: Intel Ice Lake+/AMD Zen4+)
#    - <Name>_avx2.exe   : CPU piu' vecchie (AVX2+BMI2: Intel Haswell 2013+ / AMD Zen+)
#  ⚠️ Il motore usa _pext_u64 SEMPRE (magic.cpp) -> BMI2 e' il minimo assoluto:
#     CPU pre-2013 (o senza BMI2) NON sono supportate da nessuna variante.
#     Su AMD Zen1/Zen2 il PEXT e' microcodato (lento): variante avx2 funziona ma rende meno.
#
#  4 fasi per variante:
#    1) STRUMENTA : clang-cl + ThinLTO + -fprofile-generate
#    2) ALLENA    : pgo_train.py su posizioni reali -> *.profraw
#    3) MERGE     : llvm-profdata merge -> clang.profdata
#    4) OTTIMIZZA : clang-cl + ThinLTO + -fprofile-use -> exe finale
#
#  USO:  .\build_pgo_clang.ps1                          # entrambe le varianti, Triumviratus_4.0_*
#        .\build_pgo_clang.ps1 -Arch avx2               # solo legacy
#        .\build_pgo_clang.ps1 -Arch avx512             # solo moderna
#        .\build_pgo_clang.ps1 -Name Triumviratus_4.1   # release future
#
#  RETI (runtime, non embedded): il motore carica nn-rubicon-v1.nnue (rete
#  rubicon, default) e in sua assenza fa fallback su nn-b1a57edbea57.nnue (rete
#  Stockfish); small net nn-baff1ede1f90.nnue sempre richiesta. Vanno distribuite
#  accanto all'exe.
#  ⚠️ Allena/misura a laptop SCARICO. Toolset: VS LLVM clang + llvm-profdata.
# =============================================================================
param([int]$Movetime = 0, [int]$Positions = 200, [int]$Workers = 8,
      [string]$Name = "Triumviratus_4.0",
      [ValidateSet("both","avx512","avx2")][string]$Arch = "both")
$ErrorActionPreference = "Stop"

$root    = $PSScriptRoot
# layout repo-locale (vcxproj in Triumviratus_3.0\) o layout GitHub (script accanto al vcxproj)
$proj    = if (Test-Path "$root\Triumviratus_3.0\Triumviratus_3.0.vcxproj") { "$root\Triumviratus_3.0\Triumviratus_3.0.vcxproj" }
           elseif (Test-Path "$root\Triumviratus_3.0.vcxproj") { "$root\Triumviratus_3.0.vcxproj" }
           else { throw "vcxproj non trovato (ne' layout repo ne' layout source/)" }
$projDir = Split-Path $proj
$outDir  = "$projDir\x64\Release"
$exe     = "$outDir\Triumviratus_3.4.exe"              # nome target storico del vcxproj
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

$common = @("-p:Configuration=Release","-p:Platform=x64","-p:PlatformToolset=ClangCL","-m","-nologo","-v:minimal","-p:ClangProfileLibDir=$clangLibDir")

function Build-Variant([string]$tag) {
    $suffix = "_$tag"
    $merged = "$profDir\clang_$tag.profdata"
    Write-Host "`n########  VARIANTE $tag  ->  $Name$suffix.exe  ########" -ForegroundColor Magenta

    # patch vcxproj per la variante legacy (ripristinato nel finally)
    $orig = Get-Content $proj -Raw
    try {
        if ($tag -eq "avx2") {
            $xml = $orig -replace "USE_PEXT;USE_VNNI;USE_AVX512;USE_AVX2", "USE_PEXT;USE_AVX2"
            $xml = $xml -replace "AdvancedVectorExtensions512", "AdvancedVectorExtensions2"
            Set-Content $proj $xml -Encoding UTF8
        }
        if (Test-Path $profDir) { Remove-Item "$profDir\*.profraw" -Force -ErrorAction SilentlyContinue } else { New-Item -ItemType Directory -Force -Path $profDir | Out-Null }

        Step 1 "[$tag] FASE 1: build strumentata"
        & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-generate -DCLANG_PGO_GEN" "-t:Rebuild"
        if ($LASTEXITCODE -ne 0) { throw "[$tag] build strumentata fallita" }

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
        & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-use=$merged" "-t:Rebuild"
        if ($LASTEXITCODE -ne 0) { throw "[$tag] build ottimizzata fallita" }

        Copy-Item $exe "$outDir\$Name$suffix.exe" -Force
        if (Test-Path "$root\x64\Release") { Copy-Item $exe "$root\x64\Release\$Name$suffix.exe" -Force }
        Write-Host "  -> $Name$suffix.exe pronto" -ForegroundColor Green
    } finally {
        Set-Content $proj $orig -Encoding UTF8       # vcxproj sempre ripristinato
    }

    # pulizia OutDir: solo exe + reti
    $junk = @("*.profraw","*.iobj","*.obj","*.pdb","*.ilk","*.exp","pgort*.dll",
              "*.Build.CppClean.log","*.exe.recipe","vcpkg.applocal.log","*.FileListAbsolute.txt")
    foreach ($pat in $junk) { Remove-Item (Join-Path $outDir $pat) -Force -ErrorAction SilentlyContinue }
    Remove-Item "$profDir\*.profraw" -Force -ErrorAction SilentlyContinue
}

$variants = switch ($Arch) { "both" { @("avx512","avx2") } default { @($Arch) } }
foreach ($v in $variants) { Build-Variant $v }

Write-Host "`n==== RELEASE PRONTE ====" -ForegroundColor Cyan
foreach ($v in $variants) { Write-Host "  $outDir\$Name`_$v.exe" }
Write-Host "Distribuzione: exe + nn-rubicon-v1.nnue (default) [+ nn-b1a57edbea57.nnue fallback] + nn-baff1ede1f90.nnue" -ForegroundColor Yellow
Write-Host "Requisiti CPU: avx512 = Ice Lake+/Zen4+ ; avx2 = Haswell 2013+/Zen+ (BMI2 OBBLIGATORIO per entrambe)."

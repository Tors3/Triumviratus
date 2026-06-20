# =============================================================================
#  Build RELEASE clang-cl + ThinLTO + PGO (IR-based) per TRIUMVIRATUS 4.2 (own-net).
#  Gemello di build_pgo_clang.ps1 (4.1), ma punta al vcxproj della 4.2
#  (Triumviratus_4.2\Triumviratus_4.2.vcxproj) -> target Triumviratus_4.2.exe.
#  Produce DUE varianti PGO-ottimizzate:
#    - <Name>_avx512.exe : Intel Ice Lake+/AMD Zen4+ (AVX-512+VNNI)
#    - <Name>_avx2.exe   : Haswell 2013+/Zen+ (AVX2+BMI2). BMI2 OBBLIGATORIO (PEXT).
#
#  4 fasi per variante: 1) STRUMENTA 2) ALLENA (pgo_train.py) 3) MERGE 4) OTTIMIZZA
#
#  USO:  .\build_pgo_clang_42.ps1                 # entrambe le varianti
#        .\build_pgo_clang_42.ps1 -Arch avx512    # solo moderna
#        .\build_pgo_clang_42.ps1 -Arch avx2      # solo legacy
#
#  RETI (runtime, file-based — NON embedded): la 4.2 carica nn-rubicon-v1.nnue (big,
#  own) + mini-rubicon-v1.nnue (small, own). Vanno distribuite accanto all'exe.
#  ⚠️ Allena/misura a laptop SCARICO. Toolset: VS LLVM clang + llvm-profdata.
# =============================================================================
param([int]$Movetime = 0, [int]$Positions = 200, [int]$Workers = 8,
      [string]$Name = "Triumviratus_4.2",
      [ValidateSet("both","avx512","avx2")][string]$Arch = "both")
$ErrorActionPreference = "Stop"

$root    = $PSScriptRoot
# layout repo-locale (vcxproj in Triumviratus_4.2\) o layout GitHub (script accanto al vcxproj)
$proj    = if (Test-Path "$root\Triumviratus_4.2\Triumviratus_4.2.vcxproj") { "$root\Triumviratus_4.2\Triumviratus_4.2.vcxproj" }
           elseif (Test-Path "$root\..\source\Triumviratus_4.2.vcxproj") { "$root\..\source\Triumviratus_4.2.vcxproj" }
           elseif (Test-Path "$root\Triumviratus_4.2.vcxproj") { "$root\Triumviratus_4.2.vcxproj" }
           else { throw "vcxproj 4.2 non trovato (ne' layout repo, ne' GitHub build/, ne' flat)" }
$projDir = Split-Path $proj
$outDir  = "$projDir\x64\Release"
$exe     = "$outDir\Triumviratus_4.2.exe"             # TargetName del vcxproj 4.2
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

    # variante legacy: flag clang ESPLICITI -mno-avx512* (non solo defines/arch del vcxproj)
    # + GATE anti-zmm sull'exe finale (sotto): build BOCCIATA se trova NNUE su zmm.
    $extra = ""
    if ($tag -eq "avx2") {
        $extra = " /clang:-mno-avx512f /clang:-mno-avx512bw /clang:-mno-avx512dq /clang:-mno-avx512vl /clang:-mno-avx512cd /clang:-mno-avx512vnni"
    }

    # patch vcxproj per la variante legacy (ripristinato nel finally)
    $orig = Get-Content $proj -Raw
    try {
        if ($tag -eq "avx2") {
            # Variante legacy: tolgo SIA USE_VNNI (avx2-only non ha avx512vnni/avxvnni ->
            # _mm256_dpbusd non compila) SIA USE_AVX512. Replace preciso del blocco define.
            $xml = $orig -replace "USE_PEXT;USE_VNNI;USE_AVX512;USE_AVX2", "USE_PEXT;USE_AVX2"
            $xml = $xml -replace "AdvancedVectorExtensions512", "AdvancedVectorExtensions2"
            if ($xml -match "USE_AVX512") { throw "[$tag] patch vcxproj fallita: USE_AVX512 ancora presente" }
            if ($xml -match "USE_VNNI")   { throw "[$tag] patch vcxproj fallita: USE_VNNI ancora presente" }
            Set-Content $proj $xml -Encoding UTF8
        }
        if (Test-Path $profDir) { Remove-Item "$profDir\*.profraw" -Force -ErrorAction SilentlyContinue } else { New-Item -ItemType Directory -Force -Path $profDir | Out-Null }

        Step 1 "[$tag] FASE 1: build strumentata"
        & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-generate -DCLANG_PGO_GEN$extra /clang:-ffp-contract=off" "-t:Rebuild"
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
        & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-use=$merged$extra /clang:-ffp-contract=off" "-t:Rebuild"
        if ($LASTEXITCODE -ne 0) { throw "[$tag] build ottimizzata fallita" }

        # GATE anti-AVX512 (variante legacy): vpdpbusd/vpmaddubsw/vpaddd su zmm = codice
        # EVAL nostro = crash su CPU pre-AVX512 -> build BOCCIATA.
        if ($tag -eq "avx2") {
            Step 5 "[$tag] GATE: scansione anti-AVX512 dell'exe"
            $dumpbin = Get-ChildItem "$vsRoot\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" | Select-Object -First 1 -ExpandProperty FullName
            $bad = (& $dumpbin /disasm:nobytes $exe | Select-String "vpdpbusd.*zmm|vpmaddubsw.*zmm|vpaddd.*zmm").Count
            if ($bad -gt 0) { throw "[$tag] GATE FALLITO: $bad istruzioni NNUE su zmm nella build legacy (AVX-512 leak)" }
            Write-Host "  GATE OK: 0 istruzioni NNUE-su-zmm" -ForegroundColor Green
        }

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

Write-Host "`n==== RELEASE 4.2 PRONTE ====" -ForegroundColor Cyan
foreach ($v in $variants) { Write-Host "  $outDir\$Name`_$v.exe" }
Write-Host "Distribuzione: exe + nn-rubicon-v1.nnue (big own) + mini-rubicon-v1.nnue (small own)." -ForegroundColor Yellow
Write-Host "Requisiti CPU: avx512 = Ice Lake+/Zen4+ ; avx2 = Haswell 2013+/Zen+ (BMI2 OBBLIGATORIO)."

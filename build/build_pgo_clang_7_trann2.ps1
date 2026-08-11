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
#        .\build_pgo_clang_7_trann2.ps1 -Arch all -Release -Net "Networks_Triumviratus_7\nn-legio-septima-v1.nnue"
#
#  ⚠️ -Net e' DI FATTO OBBLIGATORIO: il default qui sotto punta a una rete che non
#     esiste piu' e lo script muore con "Rete non trovata" (fallimento pulito, voluto).
#
#  MATRICE DI RELEASE (-Arch all): avx512icl, vnni512, avx512, avx2, avx2-intel, avx2-nopext.
#   avx512icl   = AVX512 + VBMI/VBMI2/BITALG -> Ice Lake+, Sapphire Rapids, Zen5
#   vnni512     = AVX512 F/BW/DQ/VL + VNNI   -> Cascade Lake, Ice Lake SP, Zen4
#   avx512      = AVX512 F/BW/DQ/VL          -> Skylake-X, Xeon W-21xx (NIENTE VNNI)
#   avx2        = AVX2 + BMI2/PEXT           -> AMD Zen3+   (= x86-64-bmi2 di Stockfish)
#   avx2-intel  = come avx2, senza `persp`   -> Intel Haswell..Rocket Lake
#   avx2-nopext = AVX2 senza PEXT            -> AMD Zen1/Zen2 (= x86-64-avx2 di Stockfish)
#
#  DUE ASSI INDIPENDENTI, e vanno tenuti distinti:
#   1) ISA     -> quali istruzioni esistono. Lo sa il preprocessore.
#   2) VENDOR  -> quale forma del codice conviene. Il preprocessore NON lo sa: si
#                 sceglie col suffisso "-intel", valido su qualsiasi target.
#  Su Zen1/Zen2 pext e' microcodato (~18 cicli contro 3): con la build sbagliata quei
#  tester perdono ~15-20% di NPS. avx2 e avx2-nopext sono node-identical -> STESSO bench,
#  e anche avx2-intel lo e': `persp` cambia solo COME si aggiorna l'accumulatore, non
#  cosa contiene. ⇒ tutta la matrice deve dare lo STESSO bench. Se non lo fa, e' un bug.
#
#  Allena/misura a laptop SCARICO. Toolset: VS LLVM clang + llvm-profdata.
# =============================================================================
param([int]$Movetime = 0, [int]$Positions = 200, [int]$Workers = 8,
      # -Times "250,1000,4000,12000": spettro di movetime del TRAINING PGO. Il default
      # di pgo_train.py e' 40-1000 ms, cioe' il regime BULLET, mentre le liste di rating
      # girano a ~2-3 s per mossa in Blitz e ~22-26 s nella 40/15 e nell'Amateur: la PGO
      # ottimizza layout e predizioni di salto per il profilo che VEDE, e finora ha visto
      # ricerche venti volte piu' corte di quelle vere.
      [string]$Times = "",
      [string]$Name = "Triumviratus_7.0",
      [string]$Net  = "Networks_Triumviratus_7\legio-ep111.nnue",
      # avx2-nopext = AVX2 SENZA pext/bmi2 (fancy-magics + dual hyperbola quintessence).
      # Serve ai tester su AMD Zen1/Zen2, dove PEXT e' microcodato (~18 cicli contro 3):
      # con la build "avx2" normale perdono ~15-20% di NPS. E' la stessa separazione che
      # fa Stockfish fra x86-64-avx2 (senza pext) e x86-64-bmi2 (con pext); il nostro
      # "avx2" corrisponde al loro "bmi2". Node-identical: stesso bench.
      # Il suffisso "-intel" e' l'asse VENDOR, ortogonale all'ISA: spegne la patch
      # `persp`, che rende su AMD (+0,63% Zen2, +1,87% Zen4) e COSTA su Intel (-1,2%
      # su Rocket Lake). Dettaglio e motivazioni in Build-Variant.
      [ValidateSet("both","all","avx512","vnni512","avx512icl","avx2","avx2-nopext",
                   "avx2-intel","avx512-intel","avx512icl-intel")][string]$Arch = "avx512",
      [string]$ExtraDefs = "",
      [switch]$Release)
$ErrorActionPreference = "Stop"
# -Release => -DTRIUMV_RELEASE: nasconde le opzioni UCI di tuning.
$reldef = if ($Release) { " -DTRIUMV_RELEASE" } else { "" }
# -ExtraDefs => define aggiuntivi, per costruire il BASELINE di una misura A/B con lo
# stesso codice e la sola patch spenta (es. "-DTRIUMV_NO_PAWN_CACHE"). Entra in TUTTE
# e due le fasi PGO: un define presente solo nella fase `use` darebbe un profilo scentrato.
if ($ExtraDefs) { $reldef += " $ExtraDefs" }

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

    # 🔴 ASSE VENDOR (6/08/2026), ortogonale all'ISA. Il suffisso "-intel" su QUALSIASI
    # target spegne `append_changed_indices_both` (la patch "persp", porting SF 7b550409).
    # Misurata con lo strumento a ordine alternato e nulli di apertura/chiusura:
    #     EPYC 7742 (Zen2)   AVX2   +0,63%   190/300
    #     Ryzen 8845HS(Zen4) AVX2   +1,87%   195/300
    #     i7-11700KF (RKL)   AVX2   -1,2%     15/300   <- Intel PERDE
    # Il pavimento dello strumento e' ~0,5% (due build PGO della stessa sorgente
    # differiscono di tanto), quindi solo persp si distingue dal rumore: pawncache,
    # hybrid e pf_small su Intel AVX2 sono tutte dentro il pavimento.
    #
    # ⚠️ Perche' a COMPILE-TIME e non con un dispatch cpuid a runtime: sotto PGO il
    # profilo si allena sul ramo preso durante il training e l'altro resta freddo
    # (marcato unlikely, spostato fuori linea). Un dispatch runtime svantaggerebbe
    # per costruzione uno dei due percorsi — e' lo stesso motivo per cui le misure
    # A/B si fanno con due build PGO e non con una leva UCI.
    $noPersp = $false
    $baseTag = $tag
    if ($tag -match '-intel$') { $noPersp = $true; $baseTag = $tag -replace '-intel$','' }
    # 🔴 6/08/2026 — `persp` si spegne anche per ISA, non solo per vendor. Misurato su
    # Zen4 8845HS con build PGO fresche e node-identical (bench 261287 su tutte):
    #     AVX2    : +2,3% con persp ACCESA (mediana stabile su 5 letture, 50 pos)
    #     AVX-512 : ~-1,3%, e la lettura OSCILLA (+0,97 / -2,60 / -1,35 / -1,82 / -1,28)
    # Su AVX-512 i tile sono piu' larghi e la pressione sui registri cambia: e' lo stesso
    # profilo della pawn cache (+1,37% AVX2, -0,11% AVX-512, spenta li'). Il segno e'
    # negativo su AMD e su Intel era gia' -1,17%: nessun vendor ha motivo di tenerla
    # accesa su AVX-512, quindi NIENTE split su quella ISA — una sola build.
    # ⚠️ Il -1,3% e' meno solido del +2,3%: 60 posizioni, mediana instabile, senza nullo
    #    di sessione. Basta a NON accendere una patch, non basterebbe ad accenderla.
    if ($baseTag -match '^(avx512|vnni512)') { $noPersp = $true }

    # PGO e' gia' specifico per questa macchina (profilato QUI), quindi tune=native
    # e' la scelta coerente. Effetto misurato sulla 6.0: dentro il rumore, tenuto
    # perche' e' un puro hint di scheduling senza effetti su memoria o correttezza.
    $extra = " /clang:-mtune=native"
    if ($noPersp) { $extra += " -DTRIUMV_NO_PERSP_BOTH" }
    # 🔴 Le MACRO servono: il codice gatta su #if defined(USE_AVX512). Senza,
    # i flag -m abilitano le istruzioni ma i percorsi restano quelli AVX2.
    #
    # 🔴 SEPARAZIONE avx512 / avx512icl (3/08/2026). Fino a oggi `avx512` definiva
    # ANCHE `USE_AVX512ICL` con -mavx512vbmi/vbmi2/bitalg, perche' era stato tarato
    # sulle VM di GCP (n2/c3 Sapphire Rapids e c3d Zen4, che hanno tutte VBMI2).
    # Ma quel binario e' quello che SPEDIAMO come "avx512": su **Skylake-X e Cascade
    # Lake** — AVX512F/BW/DQ/VL/VNNI ma SENZA VBMI/VBMI2/BITALG — muore di
    # istruzione illegale all'avvio. Sono macchine vere e diffuse fra i tester.
    # Ora i due target sono distinti, come fa Stockfish (x86-64-avx512 vs
    # x86-64-avx512icl):
    #   avx512    = F/BW/DQ/VL/VNNI            -> percorso threat SCALARE
    #   avx512icl = + VBMI/VBMI2/BITALG        -> `write_multiple_dirties` vettoriale
    #                                             (emissione delle tuple threat in
    #                                              blocco: e' il 6,5% di wall del
    #                                              catch-up specchio)
    #
    # 🔴 DOVE VANNO LE FLAG (5/08/2026). $extra entra in ClangPgoFlags, che nel vcxproj
    # sta PRIMA dei -mavx512*/-mbmi2 cablati; $archFlags entra in ClangArchFlags, che sta
    # DOPO. Le -m di clang si sovrascrivono in ordine e vince l'ultima, quindi tutto cio'
    # che DISABILITA una feature deve stare in $archFlags: prima di questa separazione i
    # -mno-avx512* del target avx2 finivano prima dei -mavx512* del vcxproj e venivano
    # annullati (le MACRO USE_AVX512 restavano comunque spente, quindi i percorsi SIMD
    # erano quelli giusti: il rischio era l'auto-vettorizzazione del compilatore).
    $archFlags = ""
    if ($baseTag -eq "avx512" -or $baseTag -eq "vnni512" -or $baseTag -eq "avx512icl") {
        $extra += " -DUSE_AVX512"
        $extra += " /clang:-mavx512f /clang:-mavx512bw /clang:-mavx512dq /clang:-mavx512vl /clang:-mbmi2"
        # 🔴 10/08/2026 — VNNI FUORI da `avx512`: Skylake-X NON ce l'ha (arriva con
        # Cascade Lake). Verificato su i9-7940X: 44 `vpdpbusd` nel binario, SIGILL
        # (exit 132) alla prima valutazione, dopo `readyok`. La verifica del 6/08 girava
        # su uno Xeon Gold 6242 = Cascade Lake, che il VNNI ce l'ha: non poteva vederlo.
        if ($baseTag -eq "vnni512" -or $baseTag -eq "avx512icl") {
            $extra += " -DUSE_VNNI /clang:-mavx512vnni"
        }
        if ($baseTag -eq "avx512icl") {
            $extra += " -DUSE_AVX512ICL"
            $extra += " /clang:-mavx512vbmi /clang:-mavx512vbmi2 /clang:-mavx512bitalg"
        }
    } elseif ($baseTag -eq "avx2" -or $baseTag -eq "avx2-nopext") {
        # Il .vcxproj ha i -mavx512* cablati in AdditionalOptions: qui li spegniamo.
        $archFlags += " -mno-avx512f -mno-avx512bw -mno-avx512dq -mno-avx512vl -mno-avx512vnni"
        if ($baseTag -eq "avx2-nopext") {
            # -UUSE_PEXT: magic.cpp -> fancy-magics, nnue/attacks.h -> DUAL_HYPERBOLA_QUINT.
            # -mno-bmi2: niente pext/pdep neanche dall'auto-vettorizzazione. BLSR/TZCNT
            # restano (vengono da /arch:AVX2 = BMI1, non da BMI2), quindi pop_lsb_bb non
            # perde nulla.
            $extra     += " -UUSE_PEXT"
            $archFlags += " -mno-bmi2"
        }
    }

    if (Test-Path $profDir) { Remove-Item "$profDir\*.profraw" -Force -ErrorAction SilentlyContinue }
    else { New-Item -ItemType Directory -Force -Path $profDir | Out-Null }

    # 🔴 SVUOTARE L'INTDIR, NON usare -t:Rebuild. Due build complete di fila nella stessa
    # cartella (FASE 1 strumentata, FASE 4 ottimizzata) rompono MSBuild in due modi:
    #   - i .tlog della prima sopravvivono e la seconda muore con "FTK1011 ... File esistente";
    #   - se si cancellano i soli .tlog, sotto -m il Clean di Rebuild toglie gli .obj e la
    #     ricompilazione viene saltata: il link poi fallisce con "could not open threads.obj".
    # Cancellare la IntDir e' un clean vero (obj + tlog insieme) e -t:Build ricostruisce.
    $intDir = "$projDir\Triumviratus_7.0\x64\Release"
    function Clear-IntDir { Remove-Item $intDir -Recurse -Force -ErrorAction SilentlyContinue }

    Clear-IntDir
    Step 1 "[$tag] FASE 1: build strumentata"
    & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-generate -DCLANG_PGO_GEN$extra$reldef /clang:-ffp-contract=off" "-p:ClangArchFlags=$archFlags" "-t:Build"
    if ($LASTEXITCODE -ne 0) { throw "[$tag] build strumentata fallita" }
    if (-not (Test-Path "$outDir\$netName")) { Copy-Item $netSrc "$outDir\$netName" -Force }

    Step 2 "[$tag] FASE 2: training ($Positions posizioni, $Workers worker)"
    $env:LLVM_PROFILE_FILE = "$profDir\prof_%p.profraw"
    Push-Location $outDir
    try {
        if ($Times) { python $trainer $exe $Movetime $book $Positions --workers $Workers --times $Times }
        else        { python $trainer $exe $Movetime $book $Positions --workers $Workers }
        if ($LASTEXITCODE -ne 0) { throw "[$tag] pgo_train.py exit $LASTEXITCODE" }
    } finally { Pop-Location; $env:LLVM_PROFILE_FILE = $null }
    $raws = @(Get-ChildItem "$profDir\*.profraw" -ErrorAction SilentlyContinue)
    if ($raws.Count -eq 0) { throw "[$tag] nessun .profraw generato" }

    Step 3 "[$tag] FASE 3: merge profili ($($raws.Count) file)"
    & $profdata merge -output="$merged" ($raws | Select-Object -ExpandProperty FullName)
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $merged)) { throw "[$tag] llvm-profdata merge fallito" }

    Clear-IntDir
    Step 4 "[$tag] FASE 4: build ottimizzata (-fprofile-use)"
    & $msbuild $proj @common "-p:ClangPgoFlags=-fprofile-use=$merged$extra$reldef /clang:-ffp-contract=off" "-p:ClangArchFlags=$archFlags" "-t:Build"
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

$variants = switch ($Arch) {
    "both" { @("avx512","avx2") }
    # matrice di release completa. avx2-nopext e' per AMD Zen1/Zen2 (pext microcodato):
    # senza, quei tester girano ~15-20% piu' lenti con la build "avx2".
    # avx2-intel = stesso ISA di avx2 ma senza `persp`, che su Intel costa -1,2%.
    # ✅ 6/08/2026 — il vendor split su AVX-512 e' CHIUSO, e la risposta e' "non serve":
    # persp e' negativa li' per entrambi i vendor, quindi `avx512` la spegne per ISA
    # (vedi Build-Variant) e una sola build copre AMD e Intel. `avx512-intel` resta
    # costruibile a mano ma sarebbe identica ad `avx512`: fuori dalla matrice.
    # Lo split resta invece necessario su AVX2: +2,3% su AMD, -1,17% su Intel.
    "all"  { @("avx512icl","avx512","avx2","avx2-intel","avx2-nopext") }
    default { @($Arch) }
}
foreach ($v in $variants) { Build-Variant $v }

Write-Host "`n==== 7.0 TRANN2 PGO PRONTA ====" -ForegroundColor Cyan
foreach ($v in $variants) { Write-Host "  $outDir\$Name`_$v.exe  (+ $netName accanto)" }

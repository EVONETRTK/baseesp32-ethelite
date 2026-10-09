# Prove automatiche sul PC (non sulla scheda) delle parti di calcolo e di
# lettura dei dati del firmware: compila ogni tests/host/test_*.c insieme ai
# .c veri di main/ (inclusi dal file di prova) e agli stub di ESP-IDF in
# tests/host/stub/, lancia le prove e stampa il riepilogo.
#
# Uso (dalla cartella del repository o da qualunque altra):
#   powershell -ExecutionPolicy Bypass -File tests\host\run.ps1
#   powershell -ExecutionPolicy Bypass -File tests\host\run.ps1 nmea_etrf   (solo test_nmea_etrf.c)
# Compilatore: zig cc. Percorso predefinito sotto, oppure variabile ZIG.
# Con PROVE_LOG=1 si vedono anche i messaggi di log del firmware.
# Termina con codice 1 se una prova fallisce o non compila.

param([string]$Solo = "")

$ErrorActionPreference = "Stop"

$Zig = $env:ZIG
if (-not $Zig) {
    $Zig = "C:\Users\Utente\tools\zig-windows-x86_64-0.13.0\zig.exe"
}
if (-not (Test-Path $Zig)) {
    Write-Host "Compilatore zig non trovato in '$Zig': impostare la variabile ZIG con il percorso di zig.exe" -ForegroundColor Red
    exit 2
}

$Here = $PSScriptRoot
$Repo = (Resolve-Path (Join-Path $Here "..\..")).Path
$Main = Join-Path $Repo "main"
$Stub = Join-Path $Here "stub"
$Out = Join-Path $Here "build"
New-Item -ItemType Directory -Force $Out | Out-Null

# --- Funzioni di validazione di web_ui.c ------------------------------------
# Estratte a ogni giro da web_ui_estrai.ps1 (condiviso con fuzz.ps1).
. (Join-Path $Here "web_ui_estrai.ps1")

# --- Compilazione ed esecuzione -------------------------------------------
$Prove = Get-ChildItem (Join-Path $Here "test_*.c") | Sort-Object Name
if ($Solo) { $Prove = $Prove | Where-Object { $_.BaseName -eq "test_$Solo" } }
if (-not $Prove) { Write-Host "Nessun file di prova trovato" -ForegroundColor Red; exit 2 }

$tot = 0; $fallite = 0; $bug = 0; $rotti = @()
$comuni = @((Join-Path $Here "prove.c"), (Join-Path $Stub "host_rt.c"))
# -fsanitize=undefined: un comportamento indefinito non passa inosservato.
# Indici fuori dai limiti noti al compilatore, puntatori nulli o
# disallineati: trap, il programma di prova si ferma ("INTERROTTA").
# Conversioni double -> int fuori scala, overflow di interi con segno, shift
# fuori misura: senza trap, fanno fallire la prova in corso con file e riga
# (gestori in prove.c), cosi' un bug noto si puo' segnare con ESEGUI_BUG.
$flag = @("cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
          "-Wno-unused-but-set-variable", "-fsanitize=undefined", "-fsanitize-trap=undefined",
          "-fno-sanitize-trap=float-cast-overflow,signed-integer-overflow,shift",
          "-include", (Join-Path $Stub "host_compat.h"),
          "-I", $Stub, "-I", $Main, "-I", $Here, "-I", $Out, "-L", $Out)
# Senza trap clang chiede al linker la libreria di UBSan, che zig su Windows
# non ha: i gestori usati sono in prove.c, basta una libreria vuota.
$vuota = Join-Path $Out "liblibclang_rt.ubsan_standalone-x86_64.a"
if (-not (Test-Path $vuota)) {
    $c = Join-Path $Out "ubsan_vuota.c"
    Set-Content -Path $c -Value "int prove_libreria_ubsan_vuota;" -Encoding ASCII
    $null = & $Zig cc -c -O1 -o (Join-Path $Out "ubsan_vuota.o") $c 2>&1
    $null = & $Zig ar rcs $vuota (Join-Path $Out "ubsan_vuota.o") 2>&1
}

foreach ($p in $Prove) {
    $nome = $p.BaseName.Substring(5)
    Write-Host ""
    Write-Host "== $nome" -ForegroundColor Cyan
    if ($nome -eq "web_ui") {
        try { New-WebUiEstratto } catch {
            Write-Host "  NON COMPILATA: $($_.Exception.Message)" -ForegroundColor Red
            $rotti += $nome; continue
        }
    }
    $exe = Join-Path $Out "$($p.BaseName).exe"
    $log = & $Zig @flag -o $exe $p.FullName @comuni 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | ForEach-Object { Write-Host "  $_" }
        Write-Host "  NON COMPILATA" -ForegroundColor Red
        $rotti += $nome; continue
    }
    $avvisi = $log | Where-Object { "$_" -match 'warning:' }
    if ($avvisi) { $log | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow } }

    $righe = & $exe 2>&1
    $codice = $LASTEXITCODE
    $riep = $null
    foreach ($r in $righe) {
        if ("$r" -match '^RIEPILOGO prove=(\d+) fallite=(\d+) bugnoti=(\d+)') { $riep = $Matches; continue }
        $col = if ("$r" -match '^\s+FALLITA') { "Red" } elseif ("$r" -match '^\s+BUG') { "Yellow" } else { "Gray" }
        Write-Host $r -ForegroundColor $col
    }
    if (-not $riep) {
        Write-Host "  INTERROTTA (codice $codice): il programma di prova si e' fermato prima della fine (crash o comportamento indefinito)" -ForegroundColor Red
        $rotti += $nome; continue
    }
    $tot += [int]$riep[1]; $fallite += [int]$riep[2]; $bug += [int]$riep[3]
}

Write-Host ""
$msg = "$tot prove, $fallite fallite"
if ($bug) { $msg += ", $bug bug noti (segnati ESEGUI_BUG: non fanno fallire il giro)" }
if ($rotti) { $msg += "; NON compilate o interrotte: $($rotti -join ', ')" }
$ok = ($fallite -eq 0 -and -not $rotti)
Write-Host $msg -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
if ($ok) { exit 0 } else { exit 1 }

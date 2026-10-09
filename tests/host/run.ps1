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
# web_ui.c dipende da server HTTP, cJSON, mbedTLS...: non si puo' compilare
# sul PC. Le poche funzioni pure si copiano qui, testo identico, in un file
# generato a ogni giro (build/web_ui_estratto.c, con #line verso web_ui.c
# per gli errori). Se una funzione cambia nome il giro lo dice.

function Get-Blocco([string[]]$Righe, [string]$Inizio, [string]$Nome) {
    for ($i = 0; $i -lt $Righe.Count; $i++) {
        if ($Righe[$i] -match $Inizio) {
            $j = $i
            while ($j -lt $Righe.Count -and -not ($Righe[$j] -match '^}')) { $j++ }
            if ($j -ge $Righe.Count) { break }
            return @("#line $($i + 1) `"main/web_ui.c`"") + $Righe[$i..$j]
        }
    }
    throw "web_ui.c: non trovo '$Nome' (cambiato nome? aggiornare run.ps1 e test_web_ui.c)"
}

function Get-PinConfig {
    # Pin del menuconfig: da sdkconfig se c'e' (configurazione vera della
    # build), altrimenti i valori predefiniti di main/Kconfig.projbuild.
    $def = [ordered]@{}
    $sdk = Join-Path $Repo "sdkconfig"
    if (Test-Path $sdk) {
        foreach ($r in Get-Content $sdk) {
            if ($r -match '^(CONFIG_BASEESP32_\w+_PIN)=(-?\d+)\s*$') { $def[$Matches[1]] = $Matches[2] }
        }
        return @{ Fonte = "sdkconfig"; Valori = $def }
    }
    $nome = $null
    foreach ($r in Get-Content (Join-Path $Main "Kconfig.projbuild")) {
        if ($r -match '^\s*config\s+(BASEESP32_\w+_PIN)\s*$') { $nome = "CONFIG_" + $Matches[1]; continue }
        if ($nome -and $r -match '^\s*default\s+(-?\d+)\s*$') { $def[$nome] = $Matches[1]; $nome = $null }
    }
    return @{ Fonte = "Kconfig.projbuild (manca sdkconfig)"; Valori = $def }
}

function New-WebUiEstratto {
    $righe = Get-Content (Join-Path $Main "web_ui.c")
    $pin = Get-PinConfig
    $txt = @(
        "// GENERATO da tests/host/run.ps1 a ogni giro: NON modificare.",
        "// Funzioni copiate senza cambiamenti da main/web_ui.c; pin da $($pin.Fonte).",
        "#include <stdio.h>",
        "#include <string.h>",
        "#include <stdbool.h>",
        "#include `"settings.h`""
    )
    foreach ($k in $pin.Valori.Keys) { $txt += "#define $k $($pin.Valori[$k])" }
    $vs = $righe | Where-Object { $_ -match '^#define VERR_SIZE ' } | Select-Object -First 1
    if (-not $vs) { throw "web_ui.c: non trovo '#define VERR_SIZE'" }
    $txt += $vs
    $txt += Get-Blocco $righe '^static void verr_set\(' "verr_set"
    $txt += Get-Blocco $righe '^static bool pin_allowed\(int pin\)' "pin_allowed"
    # typedef pin_use_t: dal "typedef struct {" che precede "} pin_use_t;"
    $fine = [Array]::FindIndex([string[]]$righe, [Predicate[string]]{ param($r) $r -match '^} pin_use_t;' })
    if ($fine -lt 0) { throw "web_ui.c: non trovo 'pin_use_t'" }
    $ini = $fine
    while ($ini -gt 0 -and -not ($righe[$ini] -match '^typedef struct')) { $ini-- }
    $txt += "#line $($ini + 1) `"main/web_ui.c`""
    $txt += $righe[$ini..$fine]
    $txt += Get-Blocco $righe '^static bool pins_conflict\(' "pins_conflict"
    $txt += Get-Blocco $righe '^static bool clean_phone_number\(' "clean_phone_number"
    $txt += Get-Blocco $righe '^static bool only_chars\(' "only_chars"
    $txt += Get-Blocco $righe '^static bool sms_number_ok\(' "sms_number_ok"
    Set-Content -Path (Join-Path $Out "web_ui_estratto.c") -Value $txt -Encoding UTF8
}

# --- Compilazione ed esecuzione -------------------------------------------
$Prove = Get-ChildItem (Join-Path $Here "test_*.c") | Sort-Object Name
if ($Solo) { $Prove = $Prove | Where-Object { $_.BaseName -eq "test_$Solo" } }
if (-not $Prove) { Write-Host "Nessun file di prova trovato" -ForegroundColor Red; exit 2 }

$tot = 0; $fallite = 0; $bug = 0; $rotti = @()
$comuni = @((Join-Path $Here "prove.c"), (Join-Path $Stub "host_rt.c"))
# -fsanitize=undefined con trap: un comportamento indefinito (overflow,
# indice fuori dai limiti noti al compilatore) ferma la prova invece di
# passare inosservato.
$flag = @("cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
          "-Wno-unused-but-set-variable", "-fsanitize=undefined", "-fsanitize-trap=undefined",
          "-include", (Join-Path $Stub "host_compat.h"),
          "-I", $Stub, "-I", $Main, "-I", $Here, "-I", $Out)

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

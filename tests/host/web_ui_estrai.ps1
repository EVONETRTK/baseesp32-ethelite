# Estrazione delle funzioni pure di main/web_ui.c in build/web_ui_estratto.c,
# usata da run.ps1 (prove) e da fuzz.ps1 (fuzzing). Va caricata con
# ". web_ui_estrai.ps1" da uno script che definisce $Repo, $Main e $Out.

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

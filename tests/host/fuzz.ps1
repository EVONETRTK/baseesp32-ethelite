# Fuzzing sul PC delle funzioni del firmware che leggono dati dall'esterno
# (ricevitore GNSS, caster NTRIP, modem, pannello): milioni di input generati
# e mutati a caso per trovare crash, letture/scritture fuori dai buffer,
# cicli infiniti e comportamenti indefiniti. Separato da run.ps1 (lento).
#
# Uso (dalla cartella del repository o da qualunque altra):
#   powershell -ExecutionPolicy Bypass -File tests\host\fuzz.ps1                 (tutti, 60 s l'uno)
#   powershell -ExecutionPolicy Bypass -File tests\host\fuzz.ps1 -Secondi 300
#   powershell -ExecutionPolicy Bypass -File tests\host\fuzz.ps1 -Solo nmea_etrf (solo fuzz\fuzz_nmea_etrf.c)
#   powershell -ExecutionPolicy Bypass -File tests\host\fuzz.ps1 -Solo gnss_fix -Riproduci tests\host\fuzz\crash\x.bin
#   powershell -ExecutionPolicy Bypass -File tests\host\fuzz.ps1 -Solo gnss_fix -Riduci tests\host\fuzz\crash\x.bin
#
# Bersagli: tests\host\fuzz\fuzz_*.c (motore in fuzz\motore.c, vedi motore.h).
# Una riga "// SORGENTI: a.c b.c" nel bersaglio aggiunge altri .c di main\
# compilati a parte (moduli con nomi statici uguali non stanno in un file solo).
# Gli errori trovati finiscono in tests\host\fuzz\crash\: <bersaglio>-<tipo>-<hash>.bin
# (l'input, da riprodurre con -Riproduci) e .txt (riepilogo con stack, file e riga).
# Uno stesso errore (stessa firma) trovato piu' volte si tiene una volta sola.
# Compilatore: zig cc, come run.ps1 (variabile ZIG). Esce con 1 se trova errori.

param(
    [int]$Secondi = 60,
    [string]$Solo = "",
    [long]$Seme = 1,
    [string]$Riproduci = "",
    [string]$Riduci = ""
)

# Continue: i messaggi di zig e del bersaglio su stderr non devono fermare lo script.
$ErrorActionPreference = "Continue"

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
$FDir = Join-Path $Here "fuzz"
$Out = Join-Path $Here "build"
$FOut = Join-Path $Out "fuzz"
$Crash = Join-Path $FDir "crash"
New-Item -ItemType Directory -Force $FOut, $Crash | Out-Null

. (Join-Path $Here "web_ui_estrai.ps1")

$Bersagli = Get-ChildItem (Join-Path $FDir "fuzz_*.c") | Sort-Object Name
if ($Solo) { $Bersagli = $Bersagli | Where-Object { $_.BaseName -eq "fuzz_$Solo" } }
if (-not $Bersagli) { Write-Host "Nessun bersaglio trovato" -ForegroundColor Red; exit 2 }

# Copertura per guidare le mutazioni (contatori dei blocchi, trace-cmp), UBSan che
# segnala e continua (gestori nel motore), stack protector.
$flag = @("cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wno-unused-function", "-Wno-unused-variable",
          "-Wno-unused-but-set-variable", "-fsanitize=undefined", "-fno-sanitize-trap=undefined",
          "-fsanitize-coverage=inline-8bit-counters,trace-cmp", "-fstack-protector-strong",
          "-fasynchronous-unwind-tables", "-fno-optimize-sibling-calls", "-fno-inline",
          "-include", (Join-Path $Stub "host_compat.h"), "-include", (Join-Path $FDir "motore.h"),
          "-I", (Join-Path $FDir "stub"), "-I", $Stub, "-I", $Main, "-I", $Here, "-I", $FDir, "-I", $Out)

# Con -fsanitize-coverage clang chiede al linker la libreria di UBSan, che zig
# su Windows non ha: i gestori li definisce motore.c, basta una libreria vuota.
$vuota = Join-Path $FOut "liblibclang_rt.ubsan_standalone-x86_64.a"
if (-not (Test-Path $vuota)) {
    $c = Join-Path $FOut "vuoto.c"
    Set-Content -Path $c -Value "int fuzz_libreria_ubsan_vuota;" -Encoding ASCII
    & $Zig cc -c -O1 -o (Join-Path $FOut "vuoto.o") $c 2>&1 | Out-Null
    & $Zig ar rcs $vuota (Join-Path $FOut "vuoto.o") 2>&1 | Out-Null
}

# Ogni .c compilato a parte (-c) e poi collegato: compilando tutto in un
# comando solo, zig 0.13 si ferma con "Associative COMDAT symbol ... does not
# exist" (contatori della copertura su Windows).
function Build-Bersaglio($b) {
    $nome = $b.BaseName.Substring(5)
    if ($nome -eq "web_ui") { New-WebUiEstratto }
    $extra = @()
    foreach ($r in Get-Content $b.FullName -TotalCount 30) {
        if ($r -match '^//\s*SORGENTI:\s*(.+)$') {
            foreach ($s in ($Matches[1] -split '\s+')) { if ($s) { $extra += (Join-Path $Main $s) } }
        }
    }
    $exe = Join-Path $FOut "$($b.BaseName).exe"
    $obj = Join-Path $FOut "obj_$nome"
    New-Item -ItemType Directory -Force $obj | Out-Null
    $src = @($b.FullName, (Join-Path $FDir "motore.c"), (Join-Path $Stub "host_rt.c")) + $extra
    $oggetti = @()
    $log = @()
    foreach ($s in $src) {
        $o = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($s) + ".o")
        $log += & $Zig @flag -c -o $o $s 2>&1
        if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host "  $_" }; return $null }
        $oggetti += $o
    }
    $log += & $Zig cc -O1 -g -L $FOut -o $exe @oggetti 2>&1
    if ($LASTEXITCODE -ne 0) {
        $log | ForEach-Object { Write-Host "  $_" }
        return $null
    }
    $avvisi = $log | Where-Object { "$_" -match 'warning:' }
    if ($avvisi) { $log | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow } }
    return $exe
}

# --- Riproduzione / riduzione di un input salvato -----------------------------
function Get-Firma($exe, $file) {
    $righe = & $exe -nome prova -riproduci $file 2>&1
    foreach ($r in $righe) { if ("$r" -match '^ERRORE tipo=(\S+) input=\d+ firma=(.+) file=') { return "$($Matches[1]) $($Matches[2])" } }
    return $null
}

if ($Riproduci -or $Riduci) {
    if ($Bersagli.Count -ne 1) { Write-Host "-Riproduci/-Riduci vogliono -Solo <bersaglio>" -ForegroundColor Red; exit 2 }
    $exe = Build-Bersaglio $Bersagli[0]
    if (-not $exe) { exit 2 }
    if ($Riproduci) {
        & $exe -nome $Bersagli[0].BaseName.Substring(5) -riproduci (Resolve-Path $Riproduci).Path
        exit $LASTEXITCODE
    }
    # Riduzione: toglie pezzi sempre piu' piccoli finche' l'errore (stessa
    # firma) resta. Scrive <file>.min.bin.
    $file = (Resolve-Path $Riduci).Path
    $firma = Get-Firma $exe $file
    if (-not $firma) { Write-Host "L'input non riproduce nessun errore" -ForegroundColor Red; exit 1 }
    Write-Host "Firma: $firma"
    [byte[]]$d = [IO.File]::ReadAllBytes($file)
    $tmp = Join-Path $FOut "riduzione.bin"
    $passo = [Math]::Max(1, [int]($d.Length / 2))
    while ($passo -ge 1) {
        $tolto = $false
        $i = 0
        while ($i -lt $d.Length) {
            $k = [Math]::Min($passo, $d.Length - $i)
            $prova = New-Object byte[] ($d.Length - $k)
            if ($i -gt 0) { [Array]::Copy($d, 0, $prova, 0, $i) }
            if ($i + $k -lt $d.Length) { [Array]::Copy($d, $i + $k, $prova, $i, $d.Length - $i - $k) }
            [IO.File]::WriteAllBytes($tmp, $prova)
            if ((Get-Firma $exe $tmp) -eq $firma) { $d = $prova; $tolto = $true } else { $i += $k }
        }
        if (-not $tolto) { $passo = [int]($passo / 2) }
    }
    $min = [IO.Path]::ChangeExtension($file, ".min.bin")
    [IO.File]::WriteAllBytes($min, $d)
    Write-Host "Input ridotto a $($d.Length) byte: $min"
    $hex = ($d | ForEach-Object { $_.ToString("x2") }) -join " "
    Write-Host "  $hex"
    exit 0
}

# --- Fuzzing -----------------------------------------------------------------------
$riepilogo = @()
$tuttiErrori = 0
foreach ($b in $Bersagli) {
    $nome = $b.BaseName.Substring(5)
    Write-Host ""
    Write-Host "== $nome ($Secondi s)" -ForegroundColor Cyan
    $exe = Build-Bersaglio $b
    if (-not $exe) {
        Write-Host "  NON COMPILATO" -ForegroundColor Red
        $riepilogo += [pscustomobject]@{ Bersaglio = $nome; Input = 0; Errori = "non compilato" }
        $tuttiErrori++
        continue
    }
    $inizio = Get-Date
    $provati = [long]0
    $giro = 0
    $firme = @{}
    # Errori gia' salvati da giri precedenti: si riconoscono e non si duplicano.
    $giaNoti = 0
    foreach ($t in Get-ChildItem (Join-Path $Crash "$nome-*.txt") -ErrorAction SilentlyContinue) {
        $tipo = $null; $firma = $null
        foreach ($r in Get-Content $t.FullName -TotalCount 4) {
            if ($r -match '^Tipo: (.+)$') { $tipo = $Matches[1] }
            if ($r -match '^Firma: (.+)$') { $firma = $Matches[1] }
        }
        if ($tipo -and $firma -and -not $firme.ContainsKey("$tipo $firma")) {
            $firme["$tipo $firma"] = [IO.Path]::ChangeExtension($t.FullName, ".bin")
            $giaNoti++
        }
    }
    if ($giaNoti) { Write-Host "  ($giaNoti errori gia' salvati in fuzz\crash da giri precedenti)" -ForegroundColor DarkYellow }
    # Dopo un crash il programma si ferma: si riparte con un altro seme
    # finche' non scade il tempo.
    while ($true) {
        $resta = $Secondi - ((Get-Date) - $inizio).TotalSeconds
        if ($resta -lt 1) { break }
        $fineGiro = $false
        $ultimoInput = [long]0
        & $exe -nome $nome -tempo ([int]$resta) -seme ($Seme + $giro) -crash $Crash 2>&1 | ForEach-Object {
            $r = "$_"
            if ($r -match '^RISULTATO .*input=(\d+)') {
                $ultimoInput = [long]$Matches[1]; $fineGiro = $true
                Write-Host "  $r" -ForegroundColor Gray
            } elseif ($r -match '^ERRORE tipo=(\S+) input=(\d+) firma=(.+) file=(.+)$') {
                $ultimoInput = [long]$Matches[2]
                $chiave = "$($Matches[1]) $($Matches[3])"
                $fileBin = $Matches[4]
                if ($firme.ContainsKey($chiave)) {
                    # gia' visto: si tiene l'input piu' corto
                    $vecchio = $firme[$chiave]
                    if ((Test-Path $fileBin) -and (Test-Path $vecchio) -and (Get-Item $fileBin).Length -lt (Get-Item $vecchio).Length) {
                        Remove-Item $vecchio, ([IO.Path]::ChangeExtension($vecchio, ".txt")) -ErrorAction SilentlyContinue
                        $firme[$chiave] = $fileBin
                    } elseif ($fileBin -ne $vecchio) {
                        Remove-Item $fileBin, ([IO.Path]::ChangeExtension($fileBin, ".txt")) -ErrorAction SilentlyContinue
                    }
                } else {
                    $firme[$chiave] = $fileBin
                    Write-Host "  ERRORE: $chiave" -ForegroundColor Red
                    Write-Host "          $fileBin" -ForegroundColor Red
                }
            } elseif ($r -match '^\s+\.\.\. ') {
                Write-Host $r -ForegroundColor DarkGray
            }
        }
        $provati += $ultimoInput
        $giro++
        if ($fineGiro) { break }
    }
    $n = $firme.Count
    $tuttiErrori += $n
    $col = if ($n) { "Red" } else { "Green" }
    Write-Host ("  {0}: {1:N0} input provati, {2} errori distinti" -f $nome, $provati, $n) -ForegroundColor $col
    if ($giaNoti) { Write-Host "  (di cui $giaNoti gia' noti da giri precedenti)" -ForegroundColor DarkYellow }
    $riepilogo += [pscustomobject]@{ Bersaglio = $nome; Input = $provati; Errori = $n }
}

Write-Host ""
Write-Host "Riepilogo del fuzzing ($Secondi s per bersaglio):" -ForegroundColor Cyan
foreach ($r in $riepilogo) {
    $col = if ("$($r.Errori)" -eq "0") { "Green" } else { "Red" }
    Write-Host ("  {0,-22} {1,14:N0} input   errori: {2}" -f $r.Bersaglio, $r.Input, $r.Errori) -ForegroundColor $col
}
if ($tuttiErrori) {
    Write-Host "Errori salvati in $Crash (file .txt con stack, file e riga; .bin = input da riprodurre con -Riproduci)" -ForegroundColor Red
    exit 1
}
Write-Host "Nessun errore trovato" -ForegroundColor Green
exit 0

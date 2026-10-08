# ComNav / SinoGNSS K922 — comandi usati dal firmware

Fonti ufficiali (sito comnavtech.com, Download Center):
- *SinoGNSS K922 GNSS Module* (scheda tecnica, versione 2025.06.17)
- *ComNav OEM Board Reference Manual* V1.8 (2019) — comandi e log
- *K-series OEM Board User Guide* — INS (cap. 4.6), prua (5.8), base (4.x)

I manuali sono precedenti al K922: **ogni comando va verificato sul modulo
vero** (il log di avvio della base mostra ogni comando mandato).

## Scheda tecnica (in breve)
- QC7820, GPS/BDS-2/BDS-3/GLONASS/Galileo/QZSS/NavIC/SBAS; Galileo E1, E5a,
  E5b, E5 AltBOC*, **E6c*** (*= solo con firmware specifico)
- PPP-HAS e PPP-B2b dichiarati: 0,1 m orizz. / 0,2 m vert., convergenza < 15 min
- RTK 8 mm + 1 ppm; doppia antenna (0,15°/1 m di base); IMU a bordo (INS)
- UART ×3, CAN (opz.), Ethernet, I2C, SPI; **modulo LGA 48 pin 16×21 mm, 3,3 V, 0,5 W**
- uscite: NMEA (GGA, GSA, GSV, RMC, HDT, VTG, GST...), RTCM 2.x/3.x, binario ComNav

## Base
| Comando | Uso |
|---|---|
| `LOG VERSIONA ONCE` | modello e firmware |
| `UNLOGALL` | spegne tutte le uscite |
| `UNDULATION USER 0` | quota del log = ellissoidica (come il pannello) |
| `FIX POSITION lat lon quota` / `FIX AUTO` | posizione fissa / automatica |
| `LOG RTCM1005B ONTIME 10`, `LOG RTCM1074B ONTIME 1` ... | messaggi RTCM3 (B finale) |
| `LOG GPGGA ONTIME 1`, `LOG GPGSV ONTIME 1` | stato nel pannello |
| `SAVECONFIG` | salva |

Risposta ai comandi (manuale): `OK! Command Accepted!` / `Error! Unidentifiable Command!`.

## Rover
`UNLOGALL`, `FIX NONE`, `UNDULATION USER 0`, `LOG GPGGA ONTIME 1` (0.2 con INS),
`LOG GPGSV ONTIME 1`, `SAVECONFIG`.

- **Prua (doppia antenna):** `LOG HEADINGA ONTIME 1` (pannello: stesso formato
  NovAtel del Bynav M21D) e `LOG GPHDT ONTIME 0.2` (AgOpenGPS).
- **INS:** `INSCONTROL ENABLE`, `SET IMUAXESTYPE 1..8` (montaggio), `SET RTKFREQ 5`,
  `SET PVTFREQ 5`, `SET SMOOTHEDDR ON`, `SAVECONFIG`, poi **riavvio del modulo**
  (`RESET`, solo quando la configurazione INS cambia). GGA qualità 6 = soluzione INS.

## Galileo HAS
- La soluzione PPP/HAS si legge dal log `BESTPOSA` (tipo `PPP_CONVERGING` / `PPP`):
  stessi campi del `PPPNAVA` Unicore (stato, tipo, lat, lon, quota, ondulazione,
  datum, sigma lat/lon/quota, stazione, età correzioni, ...).
- **Comando di attivazione HAS: NON noto** (il manuale 2019 ha solo
  `SET PPPSOURCE lband/rtcm3/bqrtcm3`). Da trovare sul modulo o chiedere a ComNav.

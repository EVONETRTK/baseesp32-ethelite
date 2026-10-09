# Valori di riferimento per le prove sul PC (tests/host).
#
# Calcoli fatti in modo indipendente dal firmware (altro linguaggio, altri
# algoritmi dove possibile) con i parametri pubblicati. Lanciare con:
#   py tests/host/strumenti/riferimenti.py
# e ricopiare nelle prove i valori stampati se si cambia un caso di verifica.
# Nessun dato privato: coordinate di punti generici, matricole inventate.

import math
import struct
import zlib

# ---------------------------------------------------------------------------
# NMEA: checksum XOR tra '$' e '*'


def nmea(body):
    cs = 0
    for ch in body:
        cs ^= ord(ch)
    return "$%s*%02X" % (body, cs)


print("== NMEA")
for b in [
    "GNGGA,123519.00,4807.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000",
    "GNGGA,123519.00,4807.0380000,N,01131.0000000,E,5,12,0.8,545.4,M,46.9,M,2.0,0000",
    "GPGGA,000001.00,3352.1234567,S,07038.7654321,W,1,08,1.2,12.3,M,-25.1,M,,",
    "GPGGA,000002.00,,,,,0,00,99.9,,,,,,",
    "GNGGA,101010.00,4100.0000000,N,01630.0000000,E,1,10,0.9,409.5000,M,40.500,M,,",
    "GNRMC,101010.00,A,4100.0000000,N,01630.0000000,E,0.01,0.0,091026,,,A,V",
    "GNGGA,101010.00,4059.9999999,N,01629.9999999,E,1,10,0.9,409.5,M,40.5,M,,",
    "GNGSV,1,1,01,05,45,120,40,1",
]:
    print(nmea(b))

# ---------------------------------------------------------------------------
# ITRF2020 -> ETRF2000, EUREF Technical Note 1 (Altamimi, versione 2024),
# EPSG 10586, epoca di riferimento 2015.0:
#   T = (53.8, 51.8, -82.2) mm      dT/dt = (0.1, 0.0, -1.7) mm/anno
#   D = 2.25 ppb                     dD/dt = 0.11 ppb/anno
#   R = (2.106, 12.740, -20.592) mas dR/dt = (0.081, 0.490, -0.792) mas/anno
#   X_E = X_I + T + D*X_I + [[0,-R3,R2],[R3,0,-R1],[-R2,R1,0]] X_I
# Geodetiche <-> ECEF qui con il metodo chiuso di Heikkinen (diverso
# dall'iterazione del firmware), ellissoide GRS80.

A = 6378137.0
F_GRS80 = 1 / 298.257222101
F_WGS84 = 1 / 298.257223563


def llh2ecef(lat, lon, h, f):
    e2 = f * (2 - f)
    la, lo = math.radians(lat), math.radians(lon)
    n = A / math.sqrt(1 - e2 * math.sin(la) ** 2)
    return ((n + h) * math.cos(la) * math.cos(lo),
            (n + h) * math.cos(la) * math.sin(lo),
            (n * (1 - e2) + h) * math.sin(la))


def ecef2llh_heikkinen(x, y, z, f):
    a = A
    b = a * (1 - f)
    e2 = f * (2 - f)
    ep2 = (a * a - b * b) / (b * b)
    p = math.hypot(x, y)
    F = 54 * b * b * z * z
    G = p * p + (1 - e2) * z * z - e2 * (a * a - b * b)
    c = e2 * e2 * F * p * p / (G ** 3)
    s = (1 + c + math.sqrt(c * c + 2 * c)) ** (1 / 3)
    P = F / (3 * (s + 1 / s + 1) ** 2 * G * G)
    Q = math.sqrt(1 + 2 * e2 * e2 * P)
    r0 = -(P * e2 * p) / (1 + Q) + math.sqrt(
        0.5 * a * a * (1 + 1 / Q) - P * (1 - e2) * z * z / (Q * (1 + Q)) - 0.5 * P * p * p)
    U = math.sqrt((p - e2 * r0) ** 2 + z * z)
    V = math.sqrt((p - e2 * r0) ** 2 + (1 - e2) * z * z)
    z0 = b * b * z / (a * V)
    h = U * (1 - b * b / (a * V))
    lat = math.atan((z + ep2 * z0) / p)
    lon = math.atan2(y, x)
    return math.degrees(lat), math.degrees(lon), h


def itrf2020_to_etrf2000(lat, lon, h, ep):
    dt = ep - 2015.0
    T = [0.0538 + 0.0001 * dt, 0.0518 + 0.0 * dt, -0.0822 - 0.0017 * dt]
    mas = math.pi / 180 / 3600 / 1000
    R = [(2.106 + 0.081 * dt) * mas, (12.740 + 0.490 * dt) * mas, (-20.592 - 0.792 * dt) * mas]
    D = (2.25 + 0.11 * dt) * 1e-9
    X = llh2ecef(lat, lon, h, F_GRS80)
    Y = [X[0] + T[0] + D * X[0] - R[2] * X[1] + R[1] * X[2],
         X[1] + T[1] + R[2] * X[0] + D * X[1] - R[0] * X[2],
         X[2] + T[2] - R[1] * X[0] + R[0] * X[1] + D * X[2]]
    return ecef2llh_heikkinen(*Y, F_GRS80)


print("== ETRF")
for (lat, lon, h, ep) in [(41.0, 16.5, 450.0, 2026.5), (45.5, 9.2, 150.0, 2026.0), (38.1, 13.4, 50.0, 2030.0)]:
    la2, lo2, h2 = itrf2020_to_etrf2000(lat, lon, h, ep)
    dn = math.radians(la2 - lat) * 6378137.0 * (1 - 0.00669438) / (1 - 0.00669438 * math.sin(math.radians(lat)) ** 2) ** 1.5
    de = math.radians(lo2 - lon) * 6378137.0 / math.sqrt(1 - 0.00669438 * math.sin(math.radians(lat)) ** 2) * math.cos(math.radians(lat))
    print("in %.1f %.1f %.1f ep %.2f -> lat %.10f lon %.10f h %.4f  dN %.4f dE %.4f dU %.4f oriz %.4f" %
          (lat, lon, h, ep, la2, lo2, h2, dn, de, h2 - h, math.hypot(dn, de)))

# Riga GGA ITRF di un punto (41 N, 16.5 E, quota 409.5 + sep 40.5 = 450 m)
la2, lo2, h2 = itrf2020_to_etrf2000(41.0, 16.5, 450.0, 2026.5)
lat_min = (la2 - 40) * 60
lon_min = (lo2 - 16) * 60
print("GGA attesa: lat 40%010.7f lon 016%010.7f alt %.4f" % (lat_min, lon_min, h2 - 40.5))
body = "GNGGA,101010.00,%02d%010.7f,N,%03d%010.7f,E,1,10,0.9,%.4f,M,40.500,M,," % (40, lat_min, 16, lon_min, h2 - 40.5)
print(nmea(body))
body = "GNRMC,101010.00,A,%02d%010.7f,N,%03d%010.7f,E,0.01,0.0,091026,,,A,V" % (40, lat_min, 16, lon_min)
print(nmea(body))

# ---------------------------------------------------------------------------
# RTCM3 1005/1006 e CRC24Q (polinomio 0x1864CFB)


def crc24q(data):
    crc = 0
    for byte in data:
        crc ^= byte << 16
        for _ in range(8):
            crc <<= 1
            if crc & 0x1000000:
                crc ^= 0x1864CFB
    return crc & 0xFFFFFF


class Bits:
    def __init__(self):
        self.v = 0
        self.n = 0

    def put(self, val, n):
        self.v = (self.v << n) | (val & ((1 << n) - 1))
        self.n += n

    def bytes(self):
        pad = (-self.n) % 8
        v = self.v << pad
        return v.to_bytes((self.n + pad) // 8, "big")


def rtcm1005(sta, x, y, z, msg=1005, height=None):
    b = Bits()
    b.put(msg, 12)
    b.put(sta, 12)
    b.put(0, 6)       # ITRF realization year
    b.put(1, 1)       # GPS
    b.put(1, 1)       # GLONASS
    b.put(1, 1)       # Galileo
    b.put(0, 1)       # reference station indicator
    b.put(round(x * 10000), 38)
    b.put(0, 1)       # single receiver oscillator
    b.put(0, 1)       # reserved
    b.put(round(y * 10000), 38)
    b.put(0, 2)       # quarter cycle
    b.put(round(z * 10000), 38)
    if msg == 1006:
        b.put(round(height * 10000), 16)
    p = b.bytes()
    hdr = bytes([0xD3, (len(p) >> 8) & 3, len(p) & 0xFF])
    c = crc24q(hdr + p)
    return hdr + p + bytes([c >> 16, (c >> 8) & 0xFF, c & 0xFF])


def cfmt(bs):
    return ", ".join("0x%02X" % v for v in bs)


print("== RTCM")
X, Y, Z = llh2ecef(41.0, 16.5, 450.0, F_WGS84)
print("ECEF 41/16.5/450: %.4f %.4f %.4f" % (X, Y, Z))
print("1005 sta 2003 (4626000.1234, 1370000.5678, 4162000.9012):")
print(cfmt(rtcm1005(2003, 4626000.1234, 1370000.5678, 4162000.9012)))
print("1006 sta 4095 (-2700000.0001, -4300000.9999, 3850000.5000) h 1.5:")
print(cfmt(rtcm1006 := rtcm1005(4095, -2700000.0001, -4300000.9999, 3850000.5, 1006, 1.5)))
# frame 1077 finto (corto) per il demultiplexer: tipo 1077 + 6 byte qualunque
b = Bits()
b.put(1077, 12)
b.put(0x123, 12)
for v in [0xAA, 0x55, 0xD3, 0x00, 0x13, 0x62]:
    b.put(v, 8)
p = b.bytes()
hdr = bytes([0xD3, 0, len(p)])
c = crc24q(hdr + p)
print("1077 finto:", cfmt(hdr + p + bytes([c >> 16, (c >> 8) & 0xFF, c & 0xFF])))
print("crc24q('123456789') = 0x%06X" % crc24q(b"123456789"))

# ---------------------------------------------------------------------------
# UBX: checksum Fletcher a 8 bit su classe, id, lunghezza, payload


def ubx(cls, mid, payload):
    body = bytes([cls, mid]) + struct.pack("<H", len(payload)) + payload
    a = b_ = 0
    for v in body:
        a = (a + v) & 0xFF
        b_ = (b_ + a) & 0xFF
    return bytes([0xB5, 0x62]) + body + bytes([a, b_])


print("== UBX")
print("ACK-ACK 06 8A:", cfmt(ubx(0x05, 0x01, bytes([0x06, 0x8A]))))
print("ACK-NAK 06 8A:", cfmt(ubx(0x05, 0x00, bytes([0x06, 0x8A]))))

# ---------------------------------------------------------------------------
# Log ASCII Unicore/NovAtel: CRC32 riflesso 0xEDB88320, valore iniziale 0,
# nessuna inversione finale (zlib.crc32 parte da 0xFFFFFFFF e inverte: si
# ricava con un'implementazione a parte).


def crc32_novatel(s):
    crc = 0
    for ch in s.encode():
        crc ^= ch
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc


print("== Unicore")
for body in [
    'PPPNAVA,COM1,0,80.0,FINE,2400,123456.000,0,0,18,0;SOL_COMPUTED,PPP_CONVERGING,41.12345678901,16.98765432101,409.5000,40.5000,WGS84,0.1234,0.2345,0.3456,"0",5.000,0.000,30,25,0,0,0,0,0,0',
    'PPPNAVA,COM1,0,80.0,FINE,2400,123457.000,0,0,18,0;INSUFFICIENT_OBS,NONE,0.00000000000,0.00000000000,0.0000,0.0000,WGS84,0.0000,0.0000,0.0000,"0",0.000,0.000,5,0,0,0,0,0,0,0',
    'BESTPOSA,COM1,0,80.0,FINE,2400,123458.000,0,0,18,0;SOL_COMPUTED,PPP,-33.50000000000,-70.25000000000,500.0000,25.0000,WGS84,0.0500,0.0600,0.0900,"0",3.000,0.000,32,28,0,0,0,0,0,0',
    'VERSIONA,COM1,0,80.0,FINE,2400,123459.000,0,0,18,0;"UM982","R4.10Build99999","COM1","ffff","2026/01/01"',
]:
    print("#%s*%08x" % (body, crc32_novatel(body)))

# ---------------------------------------------------------------------------
# Klobuchar (IS-GPS-200, figura 20-4) in semicerchi, scritto dall'ICD.

print("== Klobuchar")
alpha = [1.1176e-08, 7.4506e-09, -5.9605e-08, -5.9605e-08]
beta = [90112.0, 0.0, -196608.0, -65536.0]


def klob(t, lat_deg, lon_deg, az_deg, el_deg):
    E = el_deg / 180.0
    psi = 0.0137 / (E + 0.11) - 0.022
    phi_u = lat_deg / 180.0
    lam_u = lon_deg / 180.0
    phi_i = phi_u + psi * math.cos(math.radians(az_deg))
    phi_i = max(-0.416, min(0.416, phi_i))
    lam_i = lam_u + psi * math.sin(math.radians(az_deg)) / math.cos(phi_i * math.pi)
    phi_m = phi_i + 0.064 * math.cos((lam_i - 1.617) * math.pi)
    tt = (4.32e4 * lam_i + t) % 86400.0
    Fz = 1.0 + 16.0 * (0.53 - E) ** 3
    AMP = sum(alpha[n] * phi_m ** n for n in range(4))
    PER = sum(beta[n] * phi_m ** n for n in range(4))
    AMP = max(AMP, 0.0)
    PER = max(PER, 72000.0)
    x = 2 * math.pi * (tt - 50400.0) / PER
    if abs(x) < 1.57:
        T = Fz * (5e-9 + AMP * (1 - x * x / 2 + x ** 4 / 24))
    else:
        T = Fz * 5e-9
    return 299792458.0 * T


for args in [(50400.0 - 3600 * 1, 41.0, 16.5, 30.0, 45.0), (7200.0, 41.0, 16.5, 200.0, 20.0),
             (45000.0, 45.5, 9.2, 0.0, 90.0), (45000.0, 41.0, 16.5, 120.0, 5.0)]:
    print("klob t %.1f lat %.1f lon %.1f az %.1f el %.1f = %.9f m" % (args + (klob(*args),)))

# ---------------------------------------------------------------------------
# Posizione del satellite da effemeride (IS-GPS-200 tabella 20-IV), scritta
# dall'ICD. Effemeride inventata ma plausibile (orbita GPS).

print("== Effemeride")
eph = dict(mu=3.986005e14, toe=388800.0, toc=388800.0, f0=1.5e-5, f1=-2.0e-12, f2=0.0,
           M0=0.8, deln=4.5e-9, e=0.012, sqrtA=5153.65, OMG0=-1.2, i0=0.96, omg=0.5,
           OMGd=-8.0e-9, idot=2.0e-10, cuc=1.0e-6, cus=8.0e-6, crc=200.0, crs=20.0, cic=5.0e-8, cis=-3.0e-8)
OMGE = 7.2921151467e-5


def satpos(e, t):
    A_ = e["sqrtA"] ** 2
    n0 = math.sqrt(e["mu"] / A_ ** 3)
    tk = t - e["toe"]
    if tk > 302400:
        tk -= 604800
    if tk < -302400:
        tk += 604800
    n = n0 + e["deln"]
    M = e["M0"] + n * tk
    E = M
    for _ in range(100):
        E = M + e["e"] * math.sin(E)
    v = math.atan2(math.sqrt(1 - e["e"] ** 2) * math.sin(E), math.cos(E) - e["e"])
    phi = v + e["omg"]
    du = e["cus"] * math.sin(2 * phi) + e["cuc"] * math.cos(2 * phi)
    dr = e["crs"] * math.sin(2 * phi) + e["crc"] * math.cos(2 * phi)
    di = e["cis"] * math.sin(2 * phi) + e["cic"] * math.cos(2 * phi)
    u = phi + du
    r = A_ * (1 - e["e"] * math.cos(E)) + dr
    i = e["i0"] + di + e["idot"] * tk
    xp, yp = r * math.cos(u), r * math.sin(u)
    Om = e["OMG0"] + (e["OMGd"] - OMGE) * tk - OMGE * e["toe"]
    x = xp * math.cos(Om) - yp * math.cos(i) * math.sin(Om)
    y = xp * math.sin(Om) + yp * math.cos(i) * math.cos(Om)
    z = yp * math.sin(i)
    tc = t - e["toc"]
    dts = e["f0"] + e["f1"] * tc + e["f2"] * tc * tc + (-4.442807633e-10) * e["e"] * e["sqrtA"] * math.sin(E)
    return x, y, z, dts


for t in [388800.0, 388800.0 + 1800.0, 388800.0 - 7000.0]:
    x, y, z, dts = satpos(eph, t)
    print("t %.1f: %.4f %.4f %.4f  dts %.15e" % (t, x, y, z, dts))

# ---------------------------------------------------------------------------
# Casi limite della riscrittura NMEA in ETRF2000 (nmea_etrf.c)
print("== ETRF casi limite")
for (lat, lon, h, ep) in [(-(33 + 52.1234567 / 60), -(70 + 38.7654321 / 60), 12.3 - 25.1, 2026.5),
                          (0.00001 / 60, 0.00001 / 60, 0.0, 2026.5),
                          (41.0 + 0.5 / 60, 16.5, 450.0, 2026.5)]:
    la2, lo2, h2 = itrf2020_to_etrf2000(lat, lon, h, ep)
    print("in %.12f %.12f %.4f -> %.12f %.12f %.4f" % (lat, lon, h, la2, lo2, h2))

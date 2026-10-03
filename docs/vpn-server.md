# Accesso remoto alle basi ESP32: server WireGuard + Tailscale

Le basi ESP32 non possono usare Tailscale direttamente: non esiste un client
Tailscale per microcontrollori. Si usa quindi un ponte sul server del caster
(`evonetrtk-prod`, `rtk.evo-net.it` = 45.129.248.45):

```
base ESP32 --WireGuard--> server (10.8.0.1) <--Tailscale-- PC / telefono
 10.8.0.2                  annuncia 10.8.0.0/24 a Tailscale
```

- La base si collega al server con WireGuard. Funziona anche con la SIM
  (CGNAT), perché è la base a chiamare il server, con un keepalive ogni 25 s.
- Il server annuncia la rete delle basi (10.8.0.0/24) a Tailscale.
- Dal PC o dal telefono basta l'app Tailscale: il pannello si apre su
  `http://10.8.0.2`, da ovunque.

Provato il 03/10/2026 con un PC di casa come server (firmware 1.19.114).

## 1. Sul server (Ubuntu/Debian, come root o con sudo)

```bash
# WireGuard
sudo apt update && sudo apt install -y wireguard

# Chiavi del server (la privata non esce dal server)
umask 077
wg genkey | sudo tee /etc/wireguard/server.key | wg pubkey | sudo tee /etc/wireguard/server.pub
```

Creare `/etc/wireguard/wg0.conf` (con `sudo nano /etc/wireguard/wg0.conf`),
mettendo al posto di `<CHIAVE PRIVATA DEL SERVER>` il contenuto di
`/etc/wireguard/server.key`:

```ini
[Interface]
Address = 10.8.0.1/24
ListenPort = 51820
PrivateKey = <CHIAVE PRIVATA DEL SERVER>

[Peer]
# Base ESP32 EVONETRTK-893428
PublicKey = hDlG4zsv8u0EIEupGST/JfqGieEFyOtCWavbTG8pSkU=
AllowedIPs = 10.8.0.2/32
```

```bash
# Inoltro dei pacchetti (Tailscale -> basi)
echo 'net.ipv4.ip_forward = 1' | sudo tee /etc/sysctl.d/99-wireguard.conf
sudo sysctl -p /etc/sysctl.d/99-wireguard.conf

# Porta UDP 51820 aperta (se il server usa ufw)
sudo ufw allow 51820/udp

# Avvio, anche dopo ogni riavvio del server
sudo systemctl enable --now wg-quick@wg0

# Annuncio della rete delle basi a Tailscale
sudo tailscale set --advertise-routes=10.8.0.0/24
```

Se il fornitore del server ha un firewall nel suo pannello web, aprire lì la
porta **51820/UDP** in ingresso.

## 2. Nel pannello web di Tailscale

<https://login.tailscale.com/admin/machines>: macchina **evonetrtk-prod** →
"…" → **Edit route settings** → spuntare **10.8.0.0/24** → **Save**.

Se la stessa rete era annunciata dal PC di prova (`evonetrtk-caster`), sul PC
toglierla:

```
tailscale set --advertise-routes=
```

Poi disattivare il tunnel "wgbase" nell'app WireGuard del PC.

## 3. Nella base (pannello, scheda Rete, riquadro "Accesso remoto (VPN WireGuard)")

| Campo | Valore |
|---|---|
| Attiva la VPN | spuntato |
| Server | `rtk.evo-net.it` |
| Porta del server | `51820` |
| Chiave pubblica del server | contenuto di `/etc/wireguard/server.pub` |
| Indirizzo della base nella VPN | `10.8.0.2` |

Premere **Salva**. Entro un minuto lo stato diventa "collegata da …".

## 4. Verifica

Sul server:

```bash
sudo wg show       # "latest handshake" di pochi secondi fa = base collegata
ping -c 3 10.8.0.2
```

Dal telefono in 4G, con l'app Tailscale attiva, aprire `http://10.8.0.2`.

## Aggiungere un'altra base

1. Nella nuova base: "Genera chiavi" e copiare la chiave pubblica.
2. Sul server, in fondo a `wg0.conf`, aggiungere un nuovo blocco con
   l'indirizzo successivo:

   ```ini
   [Peer]
   # Base <matricola>
   PublicKey = <chiave pubblica della nuova base>
   AllowedIPs = 10.8.0.3/32
   ```

3. Applicare senza interrompere le altre basi:

   ```bash
   sudo bash -c 'wg syncconf wg0 <(wg-quick strip wg0)'
   ```

4. Nella nuova base: stessi dati del punto 3, ma indirizzo `10.8.0.3`.

## Sicurezza

- Con la VPN il pannello è raggiungibile da chi è nella rete Tailscale: usare
  una password del pannello robusta, diversa dalla matricola (la matricola
  compare nel nome della rete WiFi della base).
- La chiave privata di ogni base la genera la base stessa e non esce mai.
  Se una base viene rubata, basta togliere il suo blocco `[Peer]` dal server.

# Cudy TR3000 OpenWrt Bridge Guide

This document defines the complete flashing procedure, wireless WISP client bridge configuration, and GigE Vision network routing setup for the **Cudy TR3000** portable router (MediaTek MT7981B / Filogic 820).

The objective is to transparently bridge the Creality Otter Lite 3D scanner (connected via its battery-powered **Lite Bridge** handle) onto the local laboratory network (`10.30.0.0/24`) so the `creality-win10` VM can discover and stream point clouds over Wi-Fi 6 without latency or drops.

---

## 1. Network Topology & Dual-Scenario Architecture

The Creality Otter Lite 3D scanner communicates with its battery-powered **Lite Bridge** handle over Wi-Fi 6 (802.11ax, 160 MHz channel width, HE160, SSID: `<ssid>`, WPA2: `<password>`).
The Cudy TR3000 bridges this wireless connection into the wired infrastructure.

### Dual-Scenario Architecture & Hardware Port Selection

The Cudy TR3000 has two physical Ethernet ports with distinct roles depending on operating mode:
- **Port 0 (`eth0`)**: 2.5 GbE port, labeled on casing as **WAN/LAN**.
- **Port 1 (`eth1`)**: 1.0 GbE port, labeled on casing as **LAN**.

| Operating Mode | TR3000 Port | Uplink Destination | TR3000 IP Assignment | Host Routing (`bubblegum`) |
|---|---|---|---|---|
| **Scenario A: Routed Lab Infrastructure** | **WAN (`eth0`)** | Lab Router / Switch (`10.30.0.0/24`) | Static DHCP (`10.30.0.25`, `scannerAP`) | `ip route replace 192.168.193.0/24 via 10.30.0.25` |
| **Scenario B: Direct Host-Attached (Portable)** | **LAN (`eth1`)** | `bubblegum` onboard NIC (`enp0s31f6`) | Static / DHCP Server (`192.168.1.1`) | `bubblegum` gets `192.168.1.153` via DHCP Option 121 |

> [!IMPORTANT]
> **Port Rule**:
> - When connecting TR3000 to the **lab router**, always plug into the **WAN port (`eth0`)**. The router assigns TR3000 an IP on the lab subnet (`10.30.0.x`, pinned to `10.30.0.25`). Plugging the LAN port into an existing router creates an IP subnet conflict and rogue DHCP server collisions.
> - When connecting TR3000 **directly to `bubblegum`**, plug into the **LAN port (`eth1`)**. TR3000's internal DHCP server issues `bubblegum` its IP lease.

### Scenario A — Routed Lab Infrastructure
TR3000 is wired to the lab Ethernet switch (`10.30.0.0/24`) via its **WAN** port (`eth0`), receiving its static DHCP lease (`10.30.0.25`, hostname `scannerAP`). `bubblegum` is connected via 2.5 GbE Ethernet (`10.30.0.11`) and routes `192.168.193.0/24` to the TR3000 WAN IP (`10.30.0.25`).

```text
┌────────────────────────┐
│  Creality Otter Lite   │ (Lite Bridge Handle, IP: 192.168.193.1)
│ SSID: <ssid>│ WPA2: <password>
└───────────┬────────────┘
            │ 5GHz Wi-Fi 6 (HE160, 2401.9 Mbps, phy1-sta0: 192.168.193.194)
            ▼
┌────────────────────────────────────────┐
│         Cudy TR3000 Router             │
│   • OpenWrt 25.12.6 (Filogic 820)      │
│   • WAN (eth0, 2.5G): DHCP 10.30.0.25  │
│   • Hostname: scannerAP                │
└───────────┬────────────────────────────┘
            │ 2.5 GbE Cable to WAN Port (eth0)
            ▼
┌────────────────────────┐
│  Lab Switch / Gateway  │ (Home/Lab Network: 10.30.0.0/24)
└───────────┬────────────┘
            │
            ▼
┌────────────────────────┐
│   Host: `bubblegum`    │ (10.30.0.11 wired)
│  • Route: 192.168.193.0/24 via 10.30.0.25
│  • gvcp-relay.service  │ (Relays UDP 3956 GigE Vision Discovery)
└───────────┬────────────┘
            │ Incus Bridge (10.30.11.1/24)
            ▼
┌────────────────────────┐
│ VM: `creality-testing` │ (10.30.11.170)
│ • Creality Scan 4.3.3  │ (with wlanapi.dll proxy shim)
│ • Sunshine / Moonlight │ (GPU Passthrough RTX 3060 Ti)
│ • C:\CrealityProjects  │ (Fast local NVMe frame buffer)
└────────────────────────┘
```

### Scenario B — Direct Host-Attached Mode (Portable)
TR3000 is plugged directly into `bubblegum`'s onboard Ethernet port (`enp0s31f6`).
`bubblegum` connects to the home/office network via USB Wi-Fi (`wlxccbabdb5473c` on `10.30.20.11`).

```text
┌────────────────────────┐
│  Creality Otter Lite   │ (192.168.193.1)
└───────────┬────────────┘
            │ 5GHz Wi-Fi 6 (2401.9 Mbps)
            ▼
┌────────────────────────────────────────┐
│         Cudy TR3000 Router             │
│   • OpenWrt 25.12.6                    │
│   • LAN: 192.168.1.1 (DHCP Server)     │
│   • Option 121: 192.168.193.0/24 via 1.1│
│   • Option 3: Suppressed Default GW    │
└───────────┬────────────────────────────┘
            │ Ethernet cable directly to PC
            ▼
┌────────────────────────┐
│   Host: `bubblegum`    │
│   • enp0s31f6: 192.168.1.153 (via TR3000 DHCP)
│   • wlxccbabdb5473c: 10.30.20.11 (Home Wi-Fi + Moonlight)
│   • gvcp-relay.service (UDP 3956 relay)
└───────────┬────────────┘
            │ Incus Bridge (10.30.11.1/24)
            ▼
┌────────────────────────┐
│ VM: `creality-testing` │ (10.30.11.125)
└────────────────────────┘
```

---

## 2. Converting Cudy TR3000 to OpenWrt

> **Important**: The stock Cudy TR3000 firmware validates image signatures and **rejects** unsigned third-party binaries. There is **no U-Boot web recovery interface** on this device. Flashing requires a mandatory **2-step procedure** via Cudy's officially sanctioned intermediate firmware.

### 2.1 Pre-Flight: Check Your Flash Size

Check the label on the bottom of the device before downloading anything:

| Label says | Flash size | Image folder to use |
|---|---|---|
| Nothing / standard | **128 MB** | `TR3000 V1` |
| "256MB" or "256 MB Flash" | **256 MB** | `TR3000 256 MB Flash V1` |

### 2.2 Step 1 — Intermediate Firmware (Stock Web UI Flash)

The stock Cudy web panel only accepts RSA-signed images. Cudy provides an official OpenWrt-based intermediate firmware with the correct signature.

1. Visit the [Cudy OpenWrt Software Download FAQ](https://www.cudy.com/blogs/faq/openwrt-software-download) and follow the Google Drive link.
2. Download the package matching your flash size (see table above).
3. Extract the archive and locate `cudy_tr3000-v1-sysupgrade.bin` (128 MB) or the 256 MB equivalent.
4. Connect your laptop to the Cudy TR3000 via Ethernet (LAN port).
5. Log in to the stock router web panel at `http://192.168.1.1`.
6. Navigate to **Advanced → Firmware Upgrade**.
7. Upload the intermediate `.bin` file and flash.
8. Wait ~3 minutes for reboot. The router will come up at `192.168.1.1` running OpenWrt.

### 2.3 Step 2 — Upgrade to Latest Stable OpenWrt (Sysupgrade)

Once the intermediate firmware is running, the router accepts native OpenWrt sysupgrade images.

**Download** the latest stable sysupgrade image from the [OpenWrt Firmware Selector](https://firmware-selector.openwrt.org/?version=25.12.5&target=mediatek%2Ffilogic&id=cudy_tr3000-v1) or directly:

| Flash | Direct URL |
|---|---|
| 128 MB | `https://downloads.openwrt.org/releases/25.12.5/targets/mediatek/filogic/openwrt-25.12.5-mediatek-filogic-cudy_tr3000-v1-squashfs-sysupgrade.bin` |
| 256 MB | `https://downloads.openwrt.org/releases/25.12.5/targets/mediatek/filogic/openwrt-25.12.5-mediatek-filogic-cudy_tr3000-256mb-v1-squashfs-sysupgrade.bin` |

Flash via LuCI:
1. Open `http://192.168.1.1` → **System → Backup / Flash Firmware**.
2. Under **Flash new firmware image**, select the `squashfs-sysupgrade.bin`.
3. Uncheck **Keep settings** (fresh install recommended).
4. Click **Flash image** → **Proceed**.
5. Wait ~3 minutes for reboot. Router is now on OpenWrt 25.12.5.

Or flash via SSH (faster, no UI needed):
```bash
scp openwrt-25.12.5-mediatek-filogic-cudy_tr3000-v1-squashfs-sysupgrade.bin root@192.168.1.1:/tmp/
ssh root@192.168.1.1 "sysupgrade -n /tmp/openwrt-25.12.5-mediatek-filogic-cudy_tr3000-v1-squashfs-sysupgrade.bin"
```

---

## 3. Wireless Client (WISP) Configuration

Connect to OpenWrt via SSH (`ssh root@192.168.1.1`) or the LuCI Web UI and configure the 5 GHz radio (`radio0`) to join the scanner's Wi-Fi network:

### `/etc/config/wireless`
```ini
config wifi-device 'radio0'
    option type 'mac80211'
    option path 'platform/18000000.wmac'
    option channel 'auto'
    option band '5g'
    option htmode 'HE160'
    option disabled '0'

config wifi-iface 'wifinet_scanner'
    option device 'radio0'
    option mode 'sta'
    option network 'wwan'
    option ssid '<ssid>'
    option encryption 'psk2'
    option key '<password>'
```

---

## 4. Network Bridge & Subnet Assignment

Assign the Cudy TR3000 a static IP within the laboratory subnet (`10.30.0.0/24`) and bridge the wireless client interface:

### `/etc/config/network`
```ini
config interface 'loopback'
    option device 'lo'
    option proto 'static'
    option ipaddr '127.0.0.1'
    option netmask '255.0.0.0'

config device
    option name 'br-lan'
    option type 'bridge'
    list ports 'eth0'
    list ports 'eth1'

config interface 'lan'
    option device 'br-lan'
    option proto 'static'
    option ipaddr '10.30.0.25'
    option netmask '255.255.255.0'
    option gateway '10.30.0.1'
    list dns '10.30.0.1'

config interface 'wwan'
    option proto 'dhcp'
```

---

## 5. Device Discovery & GVCP Relay Protocol

Empirical packet analysis using `pktmon` revealed that Creality Otter Lite discovery does **not** rely on Apple Bonjour / mDNS. Instead, it uses the industrial **GigE Vision Control Protocol (GVCP)** broadcast on **UDP port `3956`**.

### 5.1 Discovery Mechanics
1. **GVCP Broadcast**:
   When launched, Creality Scan broadcasts an 8-byte `DISCOVERY_CMD` packet to `255.255.255.255:3956`:
   `42 11 00 02 00 00 00 01` (Key `0x4211`, Command `0x0002`).
2. **Scanner Response**:
   The scanner (`192.168.193.1`) responds with a 256-byte unicast `DISCOVERY_ACK`:
   - Manufacturer: `Orbbec`
   - Model: `Wifi Handle`
   - Serial: `BCN965102A2`
3. **Control & Video Streams**:
   Once discovered, the application connects to unicast TCP streams:
   - Port `8090`: Command & telemetry channel
   - Port `8900`: Point cloud / mesh packet stream
   - Port `8556` / `8557`: IR & RGB camera preview video streams

### 5.2 Host Routing & GVCP Relay (`gvcp-relay.py`)

#### Host Static Route (Scenario A)
Because the scanner operates on `192.168.193.0/24` behind the TR3000 bridge, `bubblegum` must route this subnet via the TR3000's WAN IP (`10.30.0.25`):
```bash
sudo ip route replace 192.168.193.0/24 via 10.30.0.25
```
To persist across reboots, add to `/etc/netplan/` or `systemd-networkd` configuration on `bubblegum`.

#### Host GVCP Relay Service
Because the VM sits behind an Incus NAT bridge (`10.30.11.0/24`), L2 UDP broadcasts cannot cross to `192.168.193.1`. The host daemon `gvcp-relay.service` resolves this:
- Intercepts UDP `3956` broadcast probes from the VM.
- Unicasts the probe to `192.168.193.1:3956`.
- Spoofs the source IP on the reply so the Windows VM receives the `DISCOVERY_ACK` as if the scanner were on its local subnet.

Systemd service deployed on `bubblegum` at `/etc/systemd/system/gvcp-relay.service`:
```ini
[Unit]
Description=GigE Vision GVCP Discovery Relay for Creality Otter Lite
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/sbin/gvcp-relay.py --iface incusbr0 --allow 10.30.11.0/24 --scanner 192.168.193.1 --spoof
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

### 5.3 Windows Client `wlanapi.dll` Proxy Shim
Creality Scan invokes the native Windows WLAN API (`wlanapi.dll`) to verify that the active network connection is Wi-Fi before allowing wireless scanning. In a virtual machine without a passthrough Wi-Fi adapter, these calls fail.

A lightweight proxy shim (`wlanapi.dll` + `wlanshim.txt`) placed in `C:\Program Files\CrealityScan\` intercepts `WlanOpenHandle`, `WlanEnumInterfaces`, and `WlanQueryInterface`, reporting a virtual 802.11ax Wi-Fi adapter connected to `<ssid>`. This satisfies Creality Scan's UI checks entirely in software.

---

## 6. Configuration Backup & 30-Second Zero-Drift Restore

Once the Cudy TR3000 is verified operational:

1. **Export Backup Tarball**:
   ```bash
   ssh root@10.30.0.25 "sysupgrade -b /tmp/cudy-tr3000-backup.tar.gz"
   scp root@10.30.0.25:/tmp/cudy-tr3000-backup.tar.gz /var/home/elquique/Development/REPOS/INCUS/bubblegum/configs/cudy-tr3000-backup.tar.gz
   ```
2. **Instant Zero-Drift Restore**:
   If the router is ever replaced or reset to factory defaults:
   ```bash
   scp /var/home/elquique/Development/REPOS/INCUS/bubblegum/configs/cudy-tr3000-backup.tar.gz root@192.168.1.1:/tmp/
   ssh root@192.168.1.1 "sysupgrade -r /tmp/cudy-tr3000-backup.tar.gz && reboot"
   ```

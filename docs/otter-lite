# Creality Otter Lite 3D Scanner Specification & Streaming Architecture (`otter-lite.md`)

This document defines the hardware specifications, operating modes, wireless Lite Bridge streaming architecture, and framerate dynamics (15 FPS vs. 30 FPS) for the **Creality Otter Lite 3D Scanner**.

---

## 1. Hardware Architecture & Capture Modalities

The Creality Otter Lite is an industrial-grade, dual-wavelength NIR (near-infrared) structured-light 3D scanner designed for high-fidelity physical geometry capture.

- **Optical Core**: Stereo NIR structured-light cameras paired with a central RGB texture camera.
- **Metric Accuracy**: Sub-millimeter spatial accuracy ($0.05\text{ mm}$ point distance) outputting vertices in **true physical millimeters** directly.
- **Dual Operating Modes**:
  1. **Tethered Mode (USB 3.0 SuperSpeed 5 Gbps)**: Direct copper connection to the workstation or laptop.
  2. **Wireless Mode (Lite Bridge Handle)**: Snaps into a battery-powered Wi-Fi 6 wireless bridge handle (`192.168.193.1`) for untethered 3-ring walk-around scans.

---

## 2. Operating Modes: Tethered (USB 3.0) vs. Wireless (Lite Bridge)

| Feature | Tethered Mode (USB 3.0) | Wireless Mode (Lite Bridge Handle) |
|---|---|---|
| **Physical Interface** | USB Type-C SuperSpeed (5 Gbps copper) | Wi-Fi 6 (802.11ax, 160 MHz HE160, 2401.9 Mbps link) |
| **Data Protocol** | Raw uncompressed sensor frames over UVC / USB | Live555 RTSP streams (H.264 / MJPEG / Y8) over IP |
| **Capture Framerate** | **20 – 30 FPS** | **15 FPS** (Default in Creality Scan) |
| **Hardware Encoder** | Bypassed (Processed on Host PC GPU) | Onboard handle SoC real-time encoder |
| **Power Source** | Host PC USB bus power | 3,400 mAh rechargeable battery handle (~2.5–3.0 hours) |
| **Optimal Use Case** | Turntable small-object scanning, bench work | Full cyclist on trainer, large vehicles, walk-around |

---

## 3. Wireless Lite Bridge Streaming Architecture

When mounted on the battery-powered Lite Bridge handle, the handle's embedded Linux SoC attaches directly to the scanner's internal camera bus and serves **three concurrent video feeds** over RTSP:

```text
┌─────────────────────────────────────────────────────────────┐
│             Creality Otter Lite 3D Scanner                  │
└──────────────────────────────┬──────────────────────────────┘
                               │ Internal Camera Bus (Raw ~1.2 Gbps)
                               ▼
┌─────────────────────────────────────────────────────────────┐
│               Lite Bridge Wireless Handle                   │
│   • IP: 192.168.193.1 (SSID: Lite Bridge-233A39)            │
│   • Embedded Linux SoC + Hardware Video Encoders            │
│   • Live555 RTSP Streaming Engine                           │
├──────────────────────────────┬──────────────────────────────┤
│ Port 8888 (RGB Color)        │ rtsp://192.168.193.1:8888/   │
│ Port 8556 (Left NIR Stereo)  │ rtsp://192.168.193.1:8556/   │
│ Port 8557 (Right NIR Stereo) │ rtsp://192.168.193.1:8557/   │
└──────────────────────────────┴──────────────────────────────┘
```

### 3.1 Discovered RTSP Endpoints & Framerate Profiles

Empirical packet analysis and firmware probing revealed that the Lite Bridge RTSP server natively accepts both **15 FPS** and **30 FPS** profile requests:

| Feed | Default Profile (15 FPS) | High-Speed Profile (30 FPS) | Protocol & Encoding | Port |
| :--- | :--- | :--- | :--- | :---: |
| **RGB Color** | `rtsp://192.168.193.1:8888/color/1280_800_15_mjpg/` | `rtsp://192.168.193.1:8888/color/1280_800_30_mjpg/` | MJPEG over RTSP | `8888` |
| **Left NIR** | `rtsp://192.168.193.1:8556/ir_l/1280_800_15_y8/` | `rtsp://192.168.193.1:8556/ir_l/1280_800_30_y8/` | Y8 Grayscale over RTSP | `8556` |
| **Right NIR** | `rtsp://192.168.193.1:8557/ir_r/1280_800_15_y8/` | `rtsp://192.168.193.1:8557/ir_r/1280_800_30_y8/` | Y8 Grayscale over RTSP | `8557` |

Both variants return `RTSP/1.0 200 OK` on `DESCRIBE` and `SETUP` calls.

---

## 4. Why Creality Scan Enforces 15 FPS over Wi-Fi

In the official Creality Scan software (`ScannerStreamSDK.dll` / `lib_orbbec_scan.dll`), the client stack detects the wireless connection as `WifiHandleComboDevice` and hardcodes the stream URL request to `_15_`.

Creality enforced this engineering trade-off for four critical reasons:

1. **Packet Jitter vs. Tracking Lock**:
   Structured light scanning is not like watching streaming video. 3D surface reconstruction requires absolute frame synchronization between the left and right NIR stereo sensors. At 30 FPS across 3 feeds, the handle must encode and transmit **90 video frames per second**. A single dropped or delayed IR packet causes stereo disparity calculation to fail, triggering an immediate **"Tracking Lost"** error. At 15 FPS (45 frames/sec total), the 66.6 ms frame window comfortably absorbs Wi-Fi transmission jitter and automatic packet retries.
2. **Thermal Dissipation in a Sealed Handle**:
   Encoding 90 frames per second of 1280×800 video on the handle's small, uncooled embedded SoC produces rapid thermal accumulation, leading to thermal throttling and Wi-Fi disconnects mid-scan.
3. **Battery Runtime Derating**:
   At 15 FPS, the handle's 3,400 mAh battery provides ~2.5–3 hours of continuous scanning. Driving the encoders at 30 FPS cuts battery runtime to under 1 hour.
4. **Scan Accuracy is Invariant to Framerate**:
   The point cloud density, spatial resolution ($0.05\text{ mm}$), and depth calculation accuracy per frame are **identical at 15 FPS and 30 FPS**. Framerate only dictates maximum physical sweeping velocity. At 15 FPS, moving the scanner in a smooth, steady, slightly slower sweeping motion provides superior tracking stability when scanning an athlete on a stationary bicycle.

---

## 5. Advanced: Activating 30 FPS over Wi-Fi

For specialized experimental setups with strong, uncontested line-of-sight Wi-Fi 6 links (e.g., via the Cudy TR3000 bridge):

### Option A: Direct Linux / Python Ingestion (Bypassing Windows VM)
Custom point cloud and computer vision tools can ingest the high-speed streams directly via OpenCV, FFmpeg, or GStreamer without Creality Scan constraints:

```python
import cv2

# Connect directly to high-speed 30 FPS RTSP streams
cap_color = cv2.VideoCapture("rtsp://192.168.193.1:8888/color/1280_800_30_mjpg/")
cap_ir_l  = cv2.VideoCapture("rtsp://192.168.193.1:8556/ir_l/1280_800_30_y8/")
cap_ir_r  = cv2.VideoCapture("rtsp://192.168.193.1:8557/ir_r/1280_800_30_y8/")

while True:
    ret, frame = cap_color.read()
    if not ret:
        break
    # Process 30 FPS video frame...
```

### Option B: Creality Scan Binary Patching
Because the ASCII strings `1280_800_15` and `1280_800_30` have the exact same length (11 bytes), binary hex-replacing `1280_800_15` with `1280_800_30` inside `C:\Program Files\CrealityScan\ScannerStreamSDK.dll` forces Creality Scan to request the 30 FPS streams over Wi-Fi.

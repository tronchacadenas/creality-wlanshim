### 1. The Discovered RTSP Stream Endpoints

The Lite Bridge handle runs a Live555 RTSP server that serves three concurrent video feeds (RGB Color, Left NIR stereo, and Right NIR stereo). Probing the firmware revealed both 15 FPS and 30 FPS profiles:

| Stream | Default Profile (15 FPS) | High-Speed Profile (30 FPS) | Port |
| :--- | :--- | :--- | :---: |
| **RGB Color** | `rtsp://192.168.193.1:8888/color/1280_800_15_mjpg/` | `rtsp://192.168.193.1:8888/color/1280_800_30_mjpg/` | `8888` |
| **Left IR** | `rtsp://192.168.193.1:8556/ir_l/1280_800_15_y8/` | `rtsp://192.168.193.1:8556/ir_l/1280_800_30_y8/` | `8556` |
| **Right IR** | `rtsp://192.168.193.1:8557/ir_r/1280_800_15_y8/` | `rtsp://192.168.193.1:8557/ir_r/1280_800_30_y8/` | `8557` |

Both endpoint formats return `RTSP/1.0 200 OK` upon receiving `DESCRIBE` and `SETUP` requests.

---

### 2. Why Creality Scan Defaults to 15 FPS

In the Creality Scan client software (`ScannerStreamSDK.dll` / `lib_orbbec_scan.dll`):
- When it detects the connection mode as Wi-Fi (`WifiHandleComboDevice`), it **hardcodes the URL request string to `_15_`**.
- There is no toggle in the Creality Scan GUI to switch Wi-Fi to 30 FPS.
- **Why Creality did this**:
  - Over Wi-Fi, the scanner streams **3 concurrent feeds**. At 30 FPS, the battery handle's small embedded SoC must encode and stream **90 frames per second** (30 color + 30 left IR + 30 right IR).
  - Creality capped it to 15 FPS (45 frames/sec total) over Wi-Fi to prevent thermal throttling of the battery handle, extend battery life beyond ~1 hour, and avoid Wi-Fi packet jitter causing "Tracking Lost" errors.
  - The advertised 30 FPS is Creality's native rate when connected directly via **tethered USB 3.0** (where raw uncompressed frames bypass the handle's encoder over 5 Gbps copper).

---

### 3. How 30 FPS Over Wi-Fi Can Be Used

1. **Custom Linux / Direct Ingestion (e.g. Python / OpenCV / GStreamer)**:
   If using the scanner outside Creality Scan (such as with the future native Linux project or custom scripts), you can connect directly to the `_30_` RTSP endpoints:
   ```python
   import cv2
   cap_color = cv2.VideoCapture("rtsp://192.168.193.1:8888/color/1280_800_30_mjpg/")
   cap_ir_l  = cv2.VideoCapture("rtsp://192.168.193.1:8556/ir_l/1280_800_30_y8/")
   cap_ir_r  = cv2.VideoCapture("rtsp://192.168.193.1:8557/ir_r/1280_800_30_y8/")
   ```
2. **Creality Scan Binary Patching**:
   Because the URL string `1280_800_15` is stored as an ASCII string inside `ScannerStreamSDK.dll` (same string length as `1280_800_30`), a binary hex replace of `1280_800_15` $\rightarrow$ `1280_800_30` forces Creality Scan to request the 30 FPS stream over Wi-Fi. (However, ensure your Cudy TR3000 Wi-Fi 6 signal is strong to prevent dropped frames).

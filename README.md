# wlanapi.dll proxy shim

A small proxy `wlanapi.dll` that logs every Windows WLAN API call an application makes and, on a machine
with **no Wi-Fi adapter** (a Windows VM, Wine, a desktop PC), presents one virtual Wi-Fi interface that is
"connected" to a Wi-Fi network name of your choosing. If the machine has real Wi-Fi hardware, every call is
passed through to the real `wlanapi.dll` untouched.

It was written for **Creality Scan** (3D-scanner software) running in a Windows VM, where the app reported
"No Device Connected" because the VM is on Ethernet while the scanner expects the PC to be on its Wi-Fi network.
It may help with similar software that gates on a Wi-Fi connection, but it is a workaround, not a fix: it only
changes what the *WLAN API* says. Whether it is enough depends on how the app checks (see
[Limitations](#limitations) and [Reading the log](#reading-the-log)).

> Unofficial and unaffiliated with Creality or any scanner vendor. Use at your own risk.

## How it works

Windows looks for `wlanapi.dll` in the application's own folder before `System32`. Put this DLL next to
`CrealityScan.exe` and the app talks to the shim instead.

- **Real Wi-Fi present:** everything is forwarded to the real `wlanapi.dll`; nothing is faked.
- **No Wi-Fi (or Wine, or `WLANSHIM_FORCE_FAKE=1`):** the shim reports one interface whose state is
  "connected", with a single visible network named after your SSID (WPA2-PSK, 5 GHz channel 36, 866 Mbit/s,
  signal 100 %). `WlanScan` and `WlanConnect` succeed.
- **Adapter binding is opt-in.** By default the virtual interface has a synthetic GUID and description, so it
  never borrows the identity of a wired adapter. If your app needs the WLAN interface to map to a real IP
  adapter, set `WLANSHIM_ADAPTER` to that adapter's name or GUID (or `auto` to pick the best working one).
  Note that a wired adapter will still look wired to the app.
- **Notifications are emulated.** If the app registers with `WlanRegisterNotification`, it receives ACM events
  for the calls it makes: `scan_complete` after `WlanScan`, and `connection_start` followed by
  `connection_complete` after `WlanConnect`. They arrive asynchronously on a helper thread, like the real
  service. Nothing fires unprompted.
- Auto-detects Wine (`ntdll!wine_get_version`) and never calls Wine's stub `wlanapi.dll`; guards against loading
  itself if placed at `system32\wlanapi.dll` (don't do that); frees its own allocations safely (it never
  inspects memory it didn't allocate).

## Files

| File | Purpose |
|---|---|
| `wlanapi_shim.cpp` | the shim (single source file) |
| `wlanapi.def` | export map |
| `build.sh` | cross-compile on Linux with MinGW-w64 |
| `install-shim.ps1` | installer / uninstaller (elevated PowerShell on the Windows machine) |
| `.github/workflows/build.yml` | CI build that produces `wlanapi.dll` and its SHA-256 |

Only the x64 build is provided, so only **64-bit** applications are affected.

## Install

1. Get `wlanapi.dll`: build it (below) or download it from a release, and put it next to `install-shim.ps1`.
2. In an **elevated PowerShell** on the Windows machine:

   ```powershell
   .\install-shim.ps1 -Ssid "YOUR-SCANNER-WIFI-NAME"
   ```

   `-AppDir "C:\Program Files\CrealityScan"` if auto-detection fails, `-StopApp` to let the script stop a
   running Creality Scan, `-NoPlugin` to skip the `CrealityScan_Data\Plugins\x86_64` copy.
   The script refuses to overwrite a `wlanapi.dll` that isn't this shim unless you pass `-Force` (it makes a
   backup first).
3. Start the app, let it search for about 30 seconds, then read the log:
   `Get-Content "$env:ProgramData\wlanshim.log"`.

Manual install: copy `wlanapi.dll` and a `wlanshim.txt` (first line = SSID) into the app folder and into
`CrealityScan_Data\Plugins\x86_64` if that folder exists.

### Uninstall

```powershell
.\install-shim.ps1 -Remove
```

Removes every copy of *this shim* under the app folder (other `wlanapi.dll` files are skipped with a warning).

## Configuration

> **Set the SSID to your own scanner's Wi-Fi network name.** The built-in default, `Lite Bridge-233A39`, is the
> name of the author's scanner. If the app compares the connected network with the scanner's name, the default
> will not match yours. The installer prints a warning when you don't pass `-Ssid`.

| Setting | Effect |
|---|---|
| `wlanshim.txt` next to the DLL | first line = SSID, 1-32 bytes, UTF-8 (a BOM and trailing whitespace are ignored) |
| env `WLANSHIM_SSID` | SSID; overrides `wlanshim.txt` |
| env `WLANSHIM_ADAPTER` | opt-in: reuse this adapter's GUID/description for the virtual interface, by friendly name (as in `ipconfig`) or GUID; `auto` = best working adapter. Unset (default) = synthetic GUID |
| env `WLANSHIM_FORCE_FAKE=1` | always use the virtual interface, even if a real Wi-Fi adapter exists |
| env `WLANSHIM_LOG=0` | disable logging |

Environment variables must be present in the application's process environment (set them system-wide with
`setx`, then restart the app).

The virtual access point's BSSID is derived from the SSID: if the name ends in `-` plus six hex digits (as
`Lite Bridge-233A39` does) those digits become the last three bytes of a locally administered MAC address;
otherwise a hash of the name is used.

## Reading the log

`%ProgramData%\wlanshim.log` (rotated to `wlanshim.log.old` at 4 MiB). It records **WLAN API calls only**.
It does not show lookups the app makes through other APIs (IP Helper, WMI, `netsh`, .NET), so an adapter
mismatch there will not appear in the log.

1. **No log file at all** → the app never loaded the shim. The Wi-Fi check probably isn't `wlanapi` (for
   example a .NET `NetworkInterface` type check or `netsh`), or the app is 32-bit, or it loads `wlanapi` with a
   System32-only search path. The shim is irrelevant; look at a packet capture instead. (Also check
   `WLANSHIM_LOG` isn't `0`, and that you can write to `%ProgramData%`.)
2. **Log shows `WlanOpenHandle` / `WlanEnumInterfaces` / `WlanQueryInterface … FAKE` and the app connects** →
   done.
3. **Log shows those calls and the app still says "No Device Connected"** → the Wi-Fi gate was passed (or was
   never the blocker); discovery itself is failing. Capture UDP traffic in the VM next. Also look for calls the
   shim answers with `ERROR_NOT_SUPPORTED` / `ERROR_NOT_FOUND` (logged as `not implemented` / `not found`):
   those are the places to improve.

The first lines show the virtual interface's identity, e.g.
`virtual interface identity: guid={…} desc='Virtual Wi-Fi Adapter (wlanapi shim)' (synthetic interface …)`.

## Limitations

- **Only the WLAN API is faked.** Apps that decide "is this Wi-Fi?" by adapter type via IP Helper, WMI, `netsh`
  or .NET `NetworkInterface.NetworkInterfaceType` still see the real (Ethernet) adapter.
- One virtual network, always "connected" to the configured SSID. `WlanDisconnect` is a no-op (the link stays
  up). The profile list is empty and `WlanGetProfile` returns not-found, the BSS list returns not-found, and
  `WlanGetInterfaceCapability` returns not-supported.
- The fake/real decision is made when the app opens its WLAN handle. A Wi-Fi adapter plugged in later is not
  picked up until the app restarts.
- Notification emulation covers scan and connect only.
- Only tried with Creality Scan so far; what works for another application depends on how it checks. If you
  try it elsewhere, the log is the first thing to look at (and worth attaching to an issue).

## Build

Linux (or WSL) with MinGW-w64:

```sh
sudo apt install -y g++-mingw-w64-x86-64
./build.sh        # writes wlanapi.dll and prints its SHA-256
```

or manually:

```sh
x86_64-w64-mingw32-g++ -std=c++14 -O2 -shared -static -static-libgcc -static-libstdc++ \
  -o wlanapi.dll wlanapi_shim.cpp wlanapi.def
```

The DLL is statically linked and imports only system DLLs (`kernel32`, `msvcrt`, …). Builds from different
compiler versions differ byte for byte, so compare against the SHA-256 published with the release you
downloaded, or build it yourself from the source.

## Security note

This works by DLL search-order proxying ("side-loading"), the same technique malware uses, so antivirus
products may flag it. It only affects the folder it is copied into (the installer never touches `System32`),
loads the real system `wlanapi.dll` by full path, and is a single readable source file — review it before
running it, and prefer building it yourself. To remove it, run the uninstaller or delete the DLL.

## License

[MIT](LICENSE)

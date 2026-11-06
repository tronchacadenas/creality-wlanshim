// wlanapi_shim.cpp - proxy wlanapi.dll that logs every call and, when the machine has no
// SPDX-License-Identifier: MIT
// real Wi-Fi interface, presents one virtual interface "connected" to a configurable SSID.
// Real Wi-Fi hardware (if present) is always passed through untouched.
//
// Config (optional): wlanshim.txt next to this DLL, first line = SSID.  Env WLANSHIM_SSID overrides.
// Other environment variables:
//   WLANSHIM_FORCE_FAKE=1   always use the virtual interface, never call the real wlanapi
//   WLANSHIM_ADAPTER=<name> OPT-IN: give the virtual interface the GUID of this network adapter (friendly name
//                           or GUID); "auto" = best up adapter with an IPv4 address. Default: synthetic GUID
//   WLANSHIM_LOG=0          disable logging
// Log: %ProgramData%\wlanshim.log (rotated to wlanshim.log.old at 4 MiB)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <stddef.h>

static_assert(sizeof(WLAN_AVAILABLE_NETWORK_LIST) >= offsetof(WLAN_AVAILABLE_NETWORK_LIST, Network) + sizeof(WLAN_AVAILABLE_NETWORK), "AVAILABLE_NETWORK_LIST must embed one item");
static_assert(sizeof(WLAN_PROFILE_INFO_LIST) >= offsetof(WLAN_PROFILE_INFO_LIST, ProfileInfo) + sizeof(WLAN_PROFILE_INFO), "PROFILE_INFO_LIST must embed one item");

// ---- constants (spelled out so we don't depend on a particular MinGW header version) ----
static const ULONG_PTR FAKE_HANDLE_BASE = 0x57A11E00;   // fake handles are BASE+1 .. BASE+MAX_FAKE_HANDLES
#define MAX_FAKE_HANDLES 64
static const DWORD SHIM_SRC_ACM = 0x00000008;           // WLAN_NOTIFICATION_SOURCE_ACM
static const DWORD ACM_SCAN_COMPLETE = 7;               // wlan_notification_acm_scan_complete
static const DWORD ACM_CONNECTION_START = 9;            // wlan_notification_acm_connection_start
static const DWORD ACM_CONNECTION_COMPLETE = 10;        // wlan_notification_acm_connection_complete
static const DOT11_PHY_TYPE PHY_VHT = (DOT11_PHY_TYPE)8; // dot11_phy_type_vht (9 would be dmg / 802.11ad)
static const DWORD SHIM_GAA_INCLUDE_GATEWAYS = 0x0080;  // GAA_FLAG_INCLUDE_GATEWAYS
static const DWORD LOG_MAX_BYTES = 4u * 1024u * 1024u;

// Layout of WLAN_CONNECTION_NOTIFICATION_DATA (wlanapi.h), declared here so it builds on any header set.
struct ShimConnNotifData {
    WLAN_CONNECTION_MODE wlanConnectionMode;
    WCHAR strProfileName[256];
    DOT11_SSID dot11Ssid;
    DOT11_BSS_TYPE dot11BssType;
    BOOL bSecurityEnabled;
    DWORD wlanReasonCode;
    DWORD dwFlags;
    WCHAR strProfileXml[1];
};
static_assert(sizeof(ShimConnNotifData) == 572, "WLAN_CONNECTION_NOTIFICATION_DATA layout");

static const GUID DEFAULT_FAKE_GUID = {0x5ca1ab1e, 0xd00d, 0x4fac, {0xad, 0xde, 0xc0, 0xff, 0xee, 0x00, 0x00, 0x01}};

static CRITICAL_SECTION g_cs;           // protects globals below, the fake-allocation list and g_conn
static CRITICAL_SECTION g_dispatch_cs;  // held while a notification callback runs
static bool g_init = false;
static bool g_force_fake = false;
static HMODULE g_real = NULL;
static HMODULE g_self = NULL;
static char g_ssid[33] = "Lite Bridge-233A39";
static UCHAR g_bssid[6] = {0x02, 0x00, 0x00, 0x23, 0x3A, 0x39};
static char g_logpath[MAX_PATH + 32] = "";
static char g_logold[MAX_PATH + 40] = "";

// Identity of the virtual interface. Bound to a real network adapter (see EnsureFakeIdentity) when one
// exists, so apps that map the WLAN interface GUID to an IP adapter find a usable one.
static GUID g_fake_guid = DEFAULT_FAKE_GUID;
static WCHAR g_fake_desc[256] = L"Virtual Wi-Fi Adapter (wlanapi shim)";
static bool g_identity_done = false;

// ---- real function pointers ----
typedef DWORD (WINAPI *P_Open)(DWORD, PVOID, PDWORD, PHANDLE);
typedef DWORD (WINAPI *P_Close)(HANDLE, PVOID);
typedef DWORD (WINAPI *P_Enum)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST*);
typedef VOID  (WINAPI *P_Free)(PVOID);
typedef DWORD (WINAPI *P_Query)(HANDLE, const GUID*, WLAN_INTF_OPCODE, PVOID, PDWORD, PVOID*, PWLAN_OPCODE_VALUE_TYPE);
typedef DWORD (WINAPI *P_Scan)(HANDLE, const GUID*, PDOT11_SSID, PWLAN_RAW_DATA, PVOID);
typedef DWORD (WINAPI *P_AvNet)(HANDLE, const GUID*, DWORD, PVOID, PWLAN_AVAILABLE_NETWORK_LIST*);
typedef DWORD (WINAPI *P_Bss)(HANDLE, const GUID*, PDOT11_SSID, DOT11_BSS_TYPE, BOOL, PVOID, PWLAN_BSS_LIST*);
typedef DWORD (WINAPI *P_Connect)(HANDLE, const GUID*, PWLAN_CONNECTION_PARAMETERS, PVOID);
typedef DWORD (WINAPI *P_Disconnect)(HANDLE, const GUID*, PVOID);
typedef DWORD (WINAPI *P_SetProf)(HANDLE, const GUID*, DWORD, LPCWSTR, LPCWSTR, BOOL, PVOID, DWORD*);
typedef DWORD (WINAPI *P_ProfList)(HANDLE, const GUID*, PVOID, PWLAN_PROFILE_INFO_LIST*);
typedef DWORD (WINAPI *P_GetProf)(HANDLE, const GUID*, LPCWSTR, PVOID, LPWSTR*, DWORD*, DWORD*);
typedef DWORD (WINAPI *P_DelProf)(HANDLE, const GUID*, LPCWSTR, PVOID);
typedef DWORD (WINAPI *P_RegNotif)(HANDLE, DWORD, BOOL, WLAN_NOTIFICATION_CALLBACK, PVOID, PVOID, PDWORD);
typedef DWORD (WINAPI *P_Reason)(DWORD, DWORD, PWCHAR, PVOID);
typedef DWORD (WINAPI *P_SetIf)(HANDLE, const GUID*, WLAN_INTF_OPCODE, DWORD, PVOID, PVOID);
typedef DWORD (WINAPI *P_Cap)(HANDLE, const GUID*, PVOID, PWLAN_INTERFACE_CAPABILITY*);
typedef ULONG (WINAPI *P_GetAdaptersAddresses)(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES, PULONG);

static struct {
    P_Open Open; P_Close Close; P_Enum Enum; P_Free Free; P_Query Query; P_Scan Scan;
    P_AvNet AvNet; P_Bss Bss; P_Connect Connect; P_Disconnect Disconnect; P_SetProf SetProf;
    P_ProfList ProfList; P_GetProf GetProf; P_DelProf DelProf; P_RegNotif RegNotif;
    P_Reason Reason; P_SetIf SetIf; P_Cap Cap;
} R;

// ---- logging ----
static void Log(const char* fmt, ...) {
    if (!g_logpath[0]) return;
    char buf[768];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d [pid %lu tid %lu] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
    if (n < 0) return;
    if (n >= (int)sizeof buf - 3) n = sizeof buf - 3;
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    if (n > (int)sizeof buf - 3) n = sizeof buf - 3;
    buf[n++] = '\r'; buf[n++] = '\n';
    EnterCriticalSection(&g_cs);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExA(g_logpath, GetFileExInfoStandard, &fad) && fad.nFileSizeHigh == 0 &&
        fad.nFileSizeLow > LOG_MAX_BYTES) {
        MoveFileExA(g_logpath, g_logold, MOVEFILE_REPLACE_EXISTING);   // best effort
    }
    HANDLE f = CreateFileA(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, buf, n, &w, NULL); CloseHandle(f); }
    LeaveCriticalSection(&g_cs);
}

// ---- fake allocations ----
// Memory we hand out for fake results is tracked in a list, so WlanFreeMemory can tell it apart from
// memory that came from the real wlanapi WITHOUT dereferencing the pointer it is given.
struct alignas(16) FakeBlock { FakeBlock* next; };
static FakeBlock* g_blocks = NULL;   // guarded by g_cs

static void* FakeAlloc(size_t n) {
    FakeBlock* b = (FakeBlock*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(FakeBlock) + n);
    if (!b) return NULL;
    EnterCriticalSection(&g_cs);
    b->next = g_blocks;
    g_blocks = b;
    LeaveCriticalSection(&g_cs);
    return (void*)(b + 1);
}

static bool FakeFree(void* p) {
    if (!p) return false;
    FakeBlock* target = (FakeBlock*)((char*)p - sizeof(FakeBlock));   // address arithmetic only, never read
    bool found = false;
    EnterCriticalSection(&g_cs);
    for (FakeBlock** pp = &g_blocks; *pp; pp = &(*pp)->next) {
        if (*pp == target) { *pp = target->next; found = true; break; }
    }
    LeaveCriticalSection(&g_cs);
    if (found) HeapFree(GetProcessHeap(), 0, target);
    return found;
}

// ---- fake handles & notification registrations ----
struct FakeConn {
    bool used;
    DWORD src;                        // registered notification sources
    WLAN_NOTIFICATION_CALLBACK cb;
    PVOID ctx;
};
static FakeConn g_conn[MAX_FAKE_HANDLES];   // guarded by g_cs

static int FakeSlot(HANDLE h) {
    ULONG_PTR v = (ULONG_PTR)h;
    if (v > FAKE_HANDLE_BASE && v <= FAKE_HANDLE_BASE + MAX_FAKE_HANDLES) return (int)(v - FAKE_HANDLE_BASE - 1);
    return -1;
}
static HANDLE FakeHandleFor(int slot) { return (HANDLE)(FAKE_HANDLE_BASE + 1 + (ULONG_PTR)slot); }

static int AllocFakeSlot() {
    int slot = -1;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < MAX_FAKE_HANDLES; i++) {
        if (!g_conn[i].used) { memset(&g_conn[i], 0, sizeof g_conn[i]); g_conn[i].used = true; slot = i; break; }
    }
    LeaveCriticalSection(&g_cs);
    return slot;
}

// A call is served by the fake backend if it carries one of our fake handles or names our fake interface.
static bool IsFake(HANDLE h, const GUID* g) {
    if (FakeSlot(h) >= 0) return true;
    if (!g) return false;
    EnterCriticalSection(&g_cs);   // g_fake_guid is written once, under g_cs, when the first fake handle is opened
    bool same = IsEqualGUID(*g, g_fake_guid) != 0;
    LeaveCriticalSection(&g_cs);
    return same;
}

// ---- notifications (fake backend) ----
// Callbacks are invoked from a helper thread, never from inside the API call that triggered them
// (the real WLAN service also calls back on its own thread, and apps may hold locks while calling in).
struct EvStep { DWORD delay_ms; DWORD code; bool with_conn_data; };
struct EvJob { int slot; int n; EvStep step[2]; };

static void SsidW(WCHAR* out, size_t cap) {
    if (!MultiByteToWideChar(CP_UTF8, 0, g_ssid, -1, out, (int)cap) && cap) out[0] = 0;
}
static void FillSsid(DOT11_SSID* s) {
    s->uSSIDLength = (ULONG)strlen(g_ssid);
    memcpy(s->ucSSID, g_ssid, s->uSSIDLength);
}

static void Dispatch(int slot, DWORD code, bool with_conn_data) {
    EnterCriticalSection(&g_dispatch_cs);
    WLAN_NOTIFICATION_CALLBACK cb = NULL; PVOID ctx = NULL; DWORD src = 0;
    EnterCriticalSection(&g_cs);
    if (g_conn[slot].used) { cb = g_conn[slot].cb; ctx = g_conn[slot].ctx; src = g_conn[slot].src; }
    LeaveCriticalSection(&g_cs);
    if (cb && (src & SHIM_SRC_ACM)) {
        WLAN_NOTIFICATION_DATA nd; memset(&nd, 0, sizeof nd);
        nd.NotificationSource = SHIM_SRC_ACM;
        nd.NotificationCode = code;
        nd.InterfaceGuid = g_fake_guid;
        ShimConnNotifData cd; memset(&cd, 0, sizeof cd);
        if (with_conn_data) {
            cd.wlanConnectionMode = wlan_connection_mode_profile;
            SsidW(cd.strProfileName, 256);
            FillSsid(&cd.dot11Ssid);
            cd.dot11BssType = dot11_BSS_type_infrastructure;
            cd.bSecurityEnabled = TRUE;
            nd.dwDataSize = sizeof cd;
            nd.pData = &cd;
        }
        Log("notify ACM code=%lu -> callback %p", (unsigned long)code, (void*)cb);
        cb(&nd, ctx);
    }
    LeaveCriticalSection(&g_dispatch_cs);
}

static DWORD WINAPI EventThread(LPVOID p) {
    EvJob* j = (EvJob*)p;
    for (int i = 0; i < j->n; i++) {
        if (j->step[i].delay_ms) Sleep(j->step[i].delay_ms);
        Dispatch(j->slot, j->step[i].code, j->step[i].with_conn_data);
    }
    HeapFree(GetProcessHeap(), 0, j);
    return 0;
}

static void PostEvents(HANDLE h, const EvStep* steps, int n) {
    int slot = FakeSlot(h);
    if (slot < 0 || n < 1 || n > 2) return;
    bool want;
    EnterCriticalSection(&g_cs);
    want = g_conn[slot].used && g_conn[slot].cb && (g_conn[slot].src & SHIM_SRC_ACM);
    LeaveCriticalSection(&g_cs);
    if (!want) return;
    EvJob* j = (EvJob*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *j);
    if (!j) return;
    j->slot = slot; j->n = n;
    for (int i = 0; i < n; i++) j->step[i] = steps[i];
    HANDLE t = CreateThread(NULL, 0, EventThread, j, 0, NULL);
    if (t) CloseHandle(t); else HeapFree(GetProcessHeap(), 0, j);
}

// ---- virtual interface identity ----
static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool ParseGuid(const char* s, GUID* g) {
    unsigned char d[16]; int n = 0, hi = -1;
    for (; *s; s++) {
        if (*s == '{' || *s == '}' || *s == '-') continue;
        int v = HexVal(*s);
        if (v < 0) return false;
        if (hi < 0) hi = v;
        else { if (n >= 16) return false; d[n++] = (unsigned char)((hi << 4) | v); hi = -1; }
    }
    if (n != 16 || hi >= 0) return false;
    g->Data1 = ((DWORD)d[0] << 24) | ((DWORD)d[1] << 16) | ((DWORD)d[2] << 8) | d[3];
    g->Data2 = (unsigned short)((d[4] << 8) | d[5]);
    g->Data3 = (unsigned short)((d[6] << 8) | d[7]);
    memcpy(g->Data4, d + 8, 8);
    return true;
}
static bool ParseGuidW(const WCHAR* w, GUID* g) {
    char tmp[64]; size_t i = 0;
    for (; w[i]; i++) { if (i >= sizeof tmp - 1 || w[i] > 127) return false; tmp[i] = (char)w[i]; }
    tmp[i] = 0;
    return ParseGuid(tmp, g);
}
static void GuidStr(const GUID& g, char* out, size_t cap) {
    snprintf(out, cap, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1,
             (unsigned)g.Data2, (unsigned)g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}
static void WideToUtf8(const WCHAR* w, char* out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (w) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    out[cap - 1] = 0;
}

static bool HasUsableIPv4(PIP_ADAPTER_ADDRESSES a) {
    for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
        if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
        const unsigned char* b = (const unsigned char*)&((const sockaddr_in*)u->Address.lpSockaddr)->sin_addr;
        if (b[0] == 0 || b[0] == 127) continue;
        if (b[0] == 169 && b[1] == 254) continue;   // link-local: not really connected
        return true;
    }
    return false;
}

// Opt-in (WLANSHIM_ADAPTER): find the network adapter the virtual Wi-Fi interface should reuse the GUID of.
// "auto" = the best "up" Ethernet/Wi-Fi adapter with a usable IPv4 address (prefer one with a default
// gateway, then the lowest metric); otherwise match by friendly name or GUID. Without the variable the
// interface keeps its synthetic GUID, so it never borrows the identity of a wired adapter.
static bool PickAdapter(GUID* guid, WCHAR* desc, size_t desc_cap, char* what, size_t what_cap) {
    WCHAR want[256]; want[0] = 0;
    DWORD wl = GetEnvironmentVariableW(L"WLANSHIM_ADAPTER", want, 256);
    bool have_want = wl > 0 && wl < 256;
    if (have_want && (_wcsicmp(want, L"none") == 0 || _wcsicmp(want, L"off") == 0)) {
        snprintf(what, what_cap, "adapter binding disabled by WLANSHIM_ADAPTER");
        return false;
    }
    if (!have_want) {   // binding to a real adapter is opt-in: the default is the purely synthetic Wi-Fi interface
        snprintf(what, what_cap, "synthetic interface (set WLANSHIM_ADAPTER to bind to an adapter)");
        return false;
    }
    if (_wcsicmp(want, L"auto") == 0) have_want = false;   // heuristic pick, see below
    WCHAR sys[MAX_PATH];
    UINT sl = GetSystemDirectoryW(sys, MAX_PATH);
    if (!sl || sl >= MAX_PATH - 20) return false;
    wcscat(sys, L"\\iphlpapi.dll");     // full path: never pick up a copy from the application folder
    HMODULE ip = LoadLibraryW(sys);
    P_GetAdaptersAddresses gaa = ip ? (P_GetAdaptersAddresses)GetProcAddress(ip, "GetAdaptersAddresses") : NULL;
    if (!gaa) { snprintf(what, what_cap, "GetAdaptersAddresses unavailable"); return false; }

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | SHIM_GAA_INCLUDE_GATEWAYS;
    ULONG len = 16384, rc = ERROR_BUFFER_OVERFLOW;
    BYTE* buf = NULL;
    for (int tries = 0; tries < 4 && rc == ERROR_BUFFER_OVERFLOW; tries++) {
        buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, len);
        if (!buf) return false;
        rc = gaa(AF_INET, flags, NULL, (PIP_ADAPTER_ADDRESSES)buf, &len);
        if (rc == ERROR_BUFFER_OVERFLOW) { HeapFree(GetProcessHeap(), 0, buf); buf = NULL; }
    }
    if (!buf) { snprintf(what, what_cap, "GetAdaptersAddresses failed (rc=%lu)", (unsigned long)rc); return false; }
    if (rc != NO_ERROR) {
        HeapFree(GetProcessHeap(), 0, buf);
        snprintf(what, what_cap, "GetAdaptersAddresses failed (rc=%lu)", (unsigned long)rc);
        return false;
    }

    PIP_ADAPTER_ADDRESSES best = NULL; GUID best_guid = DEFAULT_FAKE_GUID; int best_score = -1; ULONG best_metric = 0;
    for (PIP_ADAPTER_ADDRESSES a = (PIP_ADAPTER_ADDRESSES)buf; a; a = a->Next) {
        GUID g;
        if (!a->AdapterName || !ParseGuid(a->AdapterName, &g)) continue;
        if (have_want) {
            GUID wg;
            bool match = (a->FriendlyName && _wcsicmp(a->FriendlyName, want) == 0) ||
                         (ParseGuidW(want, &wg) && IsEqualGUID(g, wg));
            if (!match) continue;
            best = a; best_guid = g;
            break;
        }
        if (a->OperStatus != 1 /* IfOperStatusUp */) continue;
        if (a->IfType != 6 /* ethernet */ && a->IfType != 71 /* IEEE 802.11 */) continue;
        if (!HasUsableIPv4(a)) continue;
        int score = (a->IfType == 71 ? 2 : 0) + (a->FirstGatewayAddress ? 4 : 0);
        if (score > best_score || (score == best_score && a->Ipv4Metric < best_metric)) {
            best = a; best_guid = g; best_score = score; best_metric = a->Ipv4Metric;
        }
    }
    bool ok = false;
    if (best) {
        *guid = best_guid;
        if (best->Description && best->Description[0]) {
            wcsncpy(desc, best->Description, desc_cap - 1);
            desc[desc_cap - 1] = 0;
        }
        char fn[160]; WideToUtf8(best->FriendlyName, fn, sizeof fn);
        snprintf(what, what_cap, "bound to adapter '%s'", fn);
        ok = true;
    } else {
        snprintf(what, what_cap, have_want ? "WLANSHIM_ADAPTER matched no adapter" : "no suitable up adapter with an IPv4 address");
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return ok;
}

static void EnsureFakeIdentity() {
    char what[256] = "";
    bool first = false;
    EnterCriticalSection(&g_cs);
    if (!g_identity_done) {
        g_identity_done = true;
        first = true;
        GUID g; WCHAR d[256]; wcscpy(d, g_fake_desc);
        if (PickAdapter(&g, d, 256, what, sizeof what)) { g_fake_guid = g; wcscpy(g_fake_desc, d); }
    }
    LeaveCriticalSection(&g_cs);
    if (first) {
        char gs[48], ds[300];
        GuidStr(g_fake_guid, gs, sizeof gs);
        WideToUtf8(g_fake_desc, ds, sizeof ds);
        Log("virtual interface identity: guid=%s desc='%s' (%s)", gs, ds, what);
    }
}

// ---- init ----
static bool EnvFlag(const char* name, bool def) {
    char v[16];
    DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    if (n == 0 || n >= sizeof v) return def;
    if (!_stricmp(v, "1") || !_stricmp(v, "true") || !_stricmp(v, "yes") || !_stricmp(v, "on")) return true;
    if (!_stricmp(v, "0") || !_stricmp(v, "false") || !_stricmp(v, "no") || !_stricmp(v, "off")) return false;
    return def;
}

// BSSID for the virtual AP: locally administered MAC. Uses the trailing "-XXXXXX" of the SSID when it is
// six hex digits (matches what Creality-style AP names encode), otherwise a hash of the SSID.
static void MakeBssid() {
    size_t len = strlen(g_ssid);
    g_bssid[0] = 0x02; g_bssid[1] = 0; g_bssid[2] = 0;
    bool suffix = len >= 7 && g_ssid[len - 7] == '-';
    for (size_t i = len - 6; suffix && i < len; i++) if (HexVal(g_ssid[i]) < 0) suffix = false;
    if (suffix) {
        for (int i = 0; i < 3; i++)
            g_bssid[3 + i] = (UCHAR)((HexVal(g_ssid[len - 6 + 2 * i]) << 4) | HexVal(g_ssid[len - 5 + 2 * i]));
    } else {
        unsigned h = 2166136261u;                       // FNV-1a
        for (size_t i = 0; i < len; i++) { h ^= (unsigned char)g_ssid[i]; h *= 16777619u; }
        g_bssid[3] = (UCHAR)(h >> 16); g_bssid[4] = (UCHAR)(h >> 8); g_bssid[5] = (UCHAR)h;
    }
}

static void Init() {
    EnterCriticalSection(&g_cs);
    if (g_init) { LeaveCriticalSection(&g_cs); return; }

    char pd[MAX_PATH];
    DWORD l = GetEnvironmentVariableA("ProgramData", pd, sizeof pd);
    if (l == 0 || l >= sizeof pd) strcpy(pd, "C:\\");
    snprintf(g_logpath, sizeof g_logpath, "%s\\wlanshim.log", pd);
    snprintf(g_logold, sizeof g_logold, "%s.old", g_logpath);
    if (!EnvFlag("WLANSHIM_LOG", true)) g_logpath[0] = 0;

    WCHAR dir[MAX_PATH];
    DWORD dl = GetModuleFileNameW(g_self, dir, MAX_PATH);
    if (dl > 0 && dl < MAX_PATH) {
        WCHAR* s = wcsrchr(dir, L'\\');
        if (s) {
            wcscpy(s + 1, L"wlanshim.txt");
            FILE* f = _wfopen(dir, L"rb");
            if (f) {
                char line[128];
                if (fgets(line, sizeof line, f)) {
                    char* p = line;
                    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;  // UTF-8 BOM
                    size_t k = strlen(p);
                    while (k && (p[k-1] == '\r' || p[k-1] == '\n' || p[k-1] == ' ' || p[k-1] == '\t')) p[--k] = 0;
                    if (k > 32) Log("wlanshim.txt: SSID is %u bytes, max is 32 - ignored", (unsigned)k);
                    else if (k > 0) strcpy(g_ssid, p);
                }
                fclose(f);
            }
        }
    }
    char env[64];
    DWORD e = GetEnvironmentVariableA("WLANSHIM_SSID", env, sizeof env);
    if (e > 32) Log("WLANSHIM_SSID is longer than 32 bytes - ignored");
    else if (e > 0) strcpy(g_ssid, env);
    MakeBssid();

    g_force_fake = EnvFlag("WLANSHIM_FORCE_FAKE", false);
    // Under Wine, C:\windows\system32\wlanapi.dll is a builtin stub that returns dummy handle 0x1
    // with 0 interfaces. Auto-detect Wine via ntdll!wine_get_version so we never call the Wine stub.
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    bool is_wine = (ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL);
    if (is_wine) g_force_fake = true;

    if (!g_force_fake) {
        WCHAR sys[MAX_PATH];
        UINT sl = GetSystemDirectoryW(sys, MAX_PATH);
        if (sl && sl < MAX_PATH - 20) {
            wcscat(sys, L"\\wlanapi.dll");
            g_real = LoadLibraryW(sys);
            if (g_real == g_self) {
                // Prevent infinite recursion if this shim is installed directly in system32\wlanapi.dll
                FreeLibrary(g_real);
                g_real = NULL;
            }
        }
    }
#define LOADFN(field, name) R.field = g_real ? (decltype(R.field))GetProcAddress(g_real, name) : NULL
    LOADFN(Open, "WlanOpenHandle"); LOADFN(Close, "WlanCloseHandle"); LOADFN(Enum, "WlanEnumInterfaces");
    LOADFN(Free, "WlanFreeMemory"); LOADFN(Query, "WlanQueryInterface"); LOADFN(Scan, "WlanScan");
    LOADFN(AvNet, "WlanGetAvailableNetworkList"); LOADFN(Bss, "WlanGetNetworkBssList");
    LOADFN(Connect, "WlanConnect"); LOADFN(Disconnect, "WlanDisconnect"); LOADFN(SetProf, "WlanSetProfile");
    LOADFN(ProfList, "WlanGetProfileList"); LOADFN(GetProf, "WlanGetProfile"); LOADFN(DelProf, "WlanDeleteProfile");
    LOADFN(RegNotif, "WlanRegisterNotification"); LOADFN(Reason, "WlanReasonCodeToString");
    LOADFN(SetIf, "WlanSetInterface"); LOADFN(Cap, "WlanGetInterfaceCapability");
    g_init = true;
    LeaveCriticalSection(&g_cs);

    char exe[MAX_PATH] = "?";
    GetModuleFileNameA(NULL, exe, sizeof exe);
    Log("=== wlanapi shim loaded in %s | ssid='%s' | wine=%s | force_fake=%s | real wlanapi=%s ===",
        exe, g_ssid, is_wine ? "yes" : "no", g_force_fake ? "yes" : "no", g_real ? "yes" : "NO");
}

extern "C" {

DWORD WINAPI Shim_WlanOpenHandle(DWORD ver, PVOID rsv, PDWORD neg, PHANDLE ph) {
    Init();
    if (!ph) return ERROR_INVALID_PARAMETER;
    DWORD rc = ERROR_SERVICE_NOT_ACTIVE;
    if (!g_force_fake && R.Open) {
        rc = R.Open(ver, rsv, neg, ph);
        if (rc == ERROR_SUCCESS) {
            // Probe whether the system actually has >=1 physical Wi-Fi interface.
            // If WlanEnumInterfaces returns 0 interfaces (e.g. Wine stub or VM with active WlanSvc),
            // close the real handle immediately and return a fake handle so handle-only calls like
            // WlanRegisterNotification(h, ...) also take the fake path.
            PWLAN_INTERFACE_INFO_LIST probe = NULL;
            DWORD erc = R.Enum ? R.Enum(*ph, NULL, &probe) : ERROR_SERVICE_NOT_ACTIVE;
            DWORD count = (erc == ERROR_SUCCESS && probe) ? probe->dwNumberOfItems : 0;
            if (erc == ERROR_SUCCESS && probe && R.Free) R.Free(probe);
            if (count > 0) {
                Log("WlanOpenHandle(ver=%lu) -> real handle %p (%lu real interface(s))", (unsigned long)ver, *ph, (unsigned long)count);
                return rc;
            }
            if (R.Close) R.Close(*ph, NULL);
            Log("WlanOpenHandle(ver=%lu) real handle had 0 interfaces (erc=%lu) -> switching to fake handle", (unsigned long)ver, (unsigned long)erc);
        }
    }
    int slot = AllocFakeSlot();
    if (slot < 0) {
        Log("WlanOpenHandle(ver=%lu) out of fake handles", (unsigned long)ver);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    EnsureFakeIdentity();
    *ph = FakeHandleFor(slot);
    if (neg) *neg = (ver == 1) ? 1 : 2;
    Log("WlanOpenHandle(ver=%lu) real rc=%lu -> FAKE handle %p", (unsigned long)ver, (unsigned long)rc, *ph);
    return ERROR_SUCCESS;
}

DWORD WINAPI Shim_WlanCloseHandle(HANDLE h, PVOID rsv) {
    Init();
    int slot = FakeSlot(h);
    Log("WlanCloseHandle(%p) %s", h, slot >= 0 ? "FAKE" : "passthrough");
    if (slot >= 0) {
        bool was_open;
        EnterCriticalSection(&g_cs);
        was_open = g_conn[slot].used;
        memset(&g_conn[slot], 0, sizeof g_conn[slot]);
        LeaveCriticalSection(&g_cs);
        // Wait for a callback that may be running right now, so the app can free its context safely.
        // (Re-entrant: closing from inside the callback itself doesn't deadlock.)
        EnterCriticalSection(&g_dispatch_cs);
        LeaveCriticalSection(&g_dispatch_cs);
        return was_open ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
    }
    return R.Close ? R.Close(h, rsv) : ERROR_SERVICE_NOT_ACTIVE;
}

VOID WINAPI Shim_WlanFreeMemory(PVOID p) {
    Init();
    if (!p) return;
    if (FakeFree(p)) return;
    if (R.Free) R.Free(p);
}

DWORD WINAPI Shim_WlanEnumInterfaces(HANDLE h, PVOID rsv, PWLAN_INTERFACE_INFO_LIST* pp) {
    Init();
    if (!pp) return ERROR_INVALID_PARAMETER;
    if (FakeSlot(h) < 0 && R.Enum) {
        DWORD rc = R.Enum(h, rsv, pp);
        if (rc == ERROR_SUCCESS && *pp && (*pp)->dwNumberOfItems > 0) {
            Log("WlanEnumInterfaces -> %lu real interface(s), passthrough", (unsigned long)(*pp)->dwNumberOfItems);
            return rc;
        }
        if (rc == ERROR_SUCCESS && *pp && R.Free) R.Free(*pp);
        Log("WlanEnumInterfaces real rc=%lu with 0 interfaces -> FAKE", (unsigned long)rc);
    } else {
        Log("WlanEnumInterfaces -> FAKE");
    }
    EnsureFakeIdentity();
    // MinGW declares InterfaceInfo[] as a flexible array (sizeof(list) == 8), so size it explicitly.
    PWLAN_INTERFACE_INFO_LIST l = (PWLAN_INTERFACE_INFO_LIST)FakeAlloc(
        offsetof(WLAN_INTERFACE_INFO_LIST, InterfaceInfo) + 1 * sizeof(WLAN_INTERFACE_INFO));
    if (!l) return ERROR_NOT_ENOUGH_MEMORY;
    l->dwNumberOfItems = 1; l->dwIndex = 0;
    l->InterfaceInfo[0].InterfaceGuid = g_fake_guid;
    wcscpy(l->InterfaceInfo[0].strInterfaceDescription, g_fake_desc);
    l->InterfaceInfo[0].isState = wlan_interface_state_connected;
    *pp = l;
    return ERROR_SUCCESS;
}

DWORD WINAPI Shim_WlanQueryInterface(HANDLE h, const GUID* g, WLAN_INTF_OPCODE op, PVOID rsv,
                                     PDWORD size, PVOID* ppData, PWLAN_OPCODE_VALUE_TYPE vt) {
    Init();
    if (!IsFake(h, g)) {
        Log("WlanQueryInterface(op=%d) passthrough", (int)op);
        return R.Query ? R.Query(h, g, op, rsv, size, ppData, vt) : ERROR_SERVICE_NOT_ACTIVE;
    }
    Log("WlanQueryInterface(op=%d) FAKE", (int)op);
    if (!size || !ppData) return ERROR_INVALID_PARAMETER;
    if (vt) *vt = wlan_opcode_value_type_query_only;
    switch (op) {
    case wlan_intf_opcode_current_connection: {
        PWLAN_CONNECTION_ATTRIBUTES a = (PWLAN_CONNECTION_ATTRIBUTES)FakeAlloc(sizeof *a);
        if (!a) return ERROR_NOT_ENOUGH_MEMORY;
        a->isState = wlan_interface_state_connected;
        a->wlanConnectionMode = wlan_connection_mode_profile;
        SsidW(a->strProfileName, 256);
        FillSsid(&a->wlanAssociationAttributes.dot11Ssid);
        a->wlanAssociationAttributes.dot11BssType = dot11_BSS_type_infrastructure;
        memcpy(a->wlanAssociationAttributes.dot11Bssid, g_bssid, 6);
        a->wlanAssociationAttributes.dot11PhyType = PHY_VHT;
        a->wlanAssociationAttributes.uDot11PhyIndex = 0;
        a->wlanAssociationAttributes.wlanSignalQuality = 100;
        a->wlanAssociationAttributes.ulRxRate = 866000;
        a->wlanAssociationAttributes.ulTxRate = 866000;
        a->wlanSecurityAttributes.bSecurityEnabled = TRUE;
        a->wlanSecurityAttributes.bOneXEnabled = FALSE;
        a->wlanSecurityAttributes.dot11AuthAlgorithm = DOT11_AUTH_ALGO_RSNA_PSK;
        a->wlanSecurityAttributes.dot11CipherAlgorithm = DOT11_CIPHER_ALGO_CCMP;
        *size = sizeof *a; *ppData = a;
        return ERROR_SUCCESS;
    }
    case wlan_intf_opcode_interface_state: {
        int* v = (int*)FakeAlloc(sizeof(int)); if (!v) return ERROR_NOT_ENOUGH_MEMORY;
        *v = wlan_interface_state_connected; *size = sizeof(int); *ppData = v; return ERROR_SUCCESS;
    }
    case wlan_intf_opcode_rssi: {
        LONG* v = (LONG*)FakeAlloc(sizeof(LONG)); if (!v) return ERROR_NOT_ENOUGH_MEMORY;
        *v = -40; *size = sizeof(LONG); *ppData = v; return ERROR_SUCCESS;
    }
    case wlan_intf_opcode_channel_number: {
        ULONG* v = (ULONG*)FakeAlloc(sizeof(ULONG)); if (!v) return ERROR_NOT_ENOUGH_MEMORY;
        *v = 36; *size = sizeof(ULONG); *ppData = v; return ERROR_SUCCESS;
    }
    case wlan_intf_opcode_autoconf_enabled:
    case wlan_intf_opcode_background_scan_enabled: {
        BOOL* v = (BOOL*)FakeAlloc(sizeof(BOOL)); if (!v) return ERROR_NOT_ENOUGH_MEMORY;
        *v = TRUE; *size = sizeof(BOOL); *ppData = v; return ERROR_SUCCESS;
    }
    default:
        Log("  op=%d not implemented -> ERROR_NOT_SUPPORTED", (int)op);
        return ERROR_NOT_SUPPORTED;
    }
}

DWORD WINAPI Shim_WlanScan(HANDLE h, const GUID* g, PDOT11_SSID s, PWLAN_RAW_DATA d, PVOID rsv) {
    Init();
    Log("WlanScan %s", IsFake(h, g) ? "FAKE" : "passthrough");
    if (IsFake(h, g)) {
        const EvStep ev[1] = {{100, ACM_SCAN_COMPLETE, false}};
        PostEvents(h, ev, 1);
        return ERROR_SUCCESS;
    }
    return R.Scan ? R.Scan(h, g, s, d, rsv) : ERROR_SERVICE_NOT_ACTIVE;
}

DWORD WINAPI Shim_WlanGetAvailableNetworkList(HANDLE h, const GUID* g, DWORD flags, PVOID rsv,
                                              PWLAN_AVAILABLE_NETWORK_LIST* pp) {
    Init();
    if (!IsFake(h, g)) {
        Log("WlanGetAvailableNetworkList passthrough");
        return R.AvNet ? R.AvNet(h, g, flags, rsv, pp) : ERROR_SERVICE_NOT_ACTIVE;
    }
    Log("WlanGetAvailableNetworkList FAKE -> 1 network '%s'", g_ssid);
    if (!pp) return ERROR_INVALID_PARAMETER;
    PWLAN_AVAILABLE_NETWORK_LIST l = (PWLAN_AVAILABLE_NETWORK_LIST)FakeAlloc(sizeof(WLAN_AVAILABLE_NETWORK_LIST));
    if (!l) return ERROR_NOT_ENOUGH_MEMORY;
    l->dwNumberOfItems = 1; l->dwIndex = 0;
    WLAN_AVAILABLE_NETWORK* n = &l->Network[0];
    SsidW(n->strProfileName, 256);
    FillSsid(&n->dot11Ssid);
    n->dot11BssType = dot11_BSS_type_infrastructure;
    n->uNumberOfBssids = 1;
    n->bNetworkConnectable = TRUE;
    n->uNumberOfPhyTypes = 1;
    n->dot11PhyTypes[0] = PHY_VHT;
    n->wlanSignalQuality = 100;
    n->bSecurityEnabled = TRUE;
    n->dot11DefaultAuthAlgorithm = DOT11_AUTH_ALGO_RSNA_PSK;
    n->dot11DefaultCipherAlgorithm = DOT11_CIPHER_ALGO_CCMP;
    n->dwFlags = 0x1 /* CONNECTED */ | 0x2 /* HAS_PROFILE */;
    *pp = l;
    return ERROR_SUCCESS;
}

DWORD WINAPI Shim_WlanGetNetworkBssList(HANDLE h, const GUID* g, PDOT11_SSID s, DOT11_BSS_TYPE t,
                                        BOOL sec, PVOID rsv, PWLAN_BSS_LIST* pp) {
    Init();
    Log("WlanGetNetworkBssList %s", IsFake(h, g) ? "FAKE (not found)" : "passthrough");
    if (IsFake(h, g)) return ERROR_NOT_FOUND;
    return R.Bss ? R.Bss(h, g, s, t, sec, rsv, pp) : ERROR_SERVICE_NOT_ACTIVE;
}

DWORD WINAPI Shim_WlanConnect(HANDLE h, const GUID* g, PWLAN_CONNECTION_PARAMETERS p, PVOID rsv) {
    Init();
    Log("WlanConnect %s", IsFake(h, g) ? "FAKE (pretend ok)" : "passthrough");
    if (IsFake(h, g)) {
        const EvStep ev[2] = {{0, ACM_CONNECTION_START, true}, {200, ACM_CONNECTION_COMPLETE, true}};
        PostEvents(h, ev, 2);
        return ERROR_SUCCESS;
    }
    return R.Connect ? R.Connect(h, g, p, rsv) : ERROR_SERVICE_NOT_ACTIVE;
}

DWORD WINAPI Shim_WlanDisconnect(HANDLE h, const GUID* g, PVOID rsv) {
    Init();
    Log("WlanDisconnect %s", IsFake(h, g) ? "FAKE (no-op: the virtual link stays up)" : "passthrough");
    if (IsFake(h, g)) return ERROR_SUCCESS;
    return R.Disconnect ? R.Disconnect(h, g, rsv) : ERROR_SERVICE_NOT_ACTIVE;
}

DWORD WINAPI Shim_WlanSetProfile(HANDLE h, const GUID* g, DWORD flags, LPCWSTR xml, LPCWSTR sec,
                                 BOOL ow, PVOID rsv, DWORD* reason) {
    Init();
    Log("WlanSetProfile %s", IsFake(h, g) ? "FAKE" : "passthrough");
    if (IsFake(h, g)) { if (reason) *reason = 0; return ERROR_SUCCESS; }
    return R.SetProf ? R.SetProf(h, g, flags, xml, sec, ow, rsv, reason) : ERROR_SERVICE_NOT_ACTIVE;
}

DWORD WINAPI Shim_WlanGetProfileList(HANDLE h, const GUID* g, PVOID rsv, PWLAN_PROFILE_INFO_LIST* pp) {
    Init();
    if (!IsFake(h, g)) {
        Log("WlanGetProfileList passthrough");
        return R.ProfList ? R.ProfList(h, g, rsv, pp) : ERROR_SERVICE_NOT_ACTIVE;
    }
    Log("WlanGetProfileList FAKE (empty)");
    if (!pp) return ERROR_INVALID_PARAMETER;
    PWLAN_PROFILE_INFO_LIST l = (PWLAN_PROFILE_INFO_LIST)FakeAlloc(sizeof(WLAN_PROFILE_INFO_LIST));
    if (!l) return ERROR_NOT_ENOUGH_MEMORY;
    *pp = l;
    return ERROR_SUCCESS;
}

DWORD WINAPI Shim_WlanGetProfile(HANDLE h, const GUID* g, LPCWSTR name, PVOID rsv, LPWSTR* xml,
                                 DWORD* flags, DWORD* access) {
    Init();
    Log("WlanGetProfile %s", IsFake(h, g) ? "FAKE (not found)" : "passthrough");
    if (IsFake(h, g)) return ERROR_NOT_FOUND;
    return R.GetProf ? R.GetProf(h, g, name, rsv, xml, flags, access) : ERROR_SERVICE_NOT_ACTIVE;
}

DWORD WINAPI Shim_WlanDeleteProfile(HANDLE h, const GUID* g, LPCWSTR name, PVOID rsv) {
    Init();
    Log("WlanDeleteProfile %s", IsFake(h, g) ? "FAKE" : "passthrough");
    if (IsFake(h, g)) return ERROR_SUCCESS;
    return R.DelProf ? R.DelProf(h, g, name, rsv) : ERROR_SERVICE_NOT_ACTIVE;
}

DWORD WINAPI Shim_WlanRegisterNotification(HANDLE h, DWORD src, BOOL ign, WLAN_NOTIFICATION_CALLBACK cb,
                                           PVOID ctx, PVOID rsv, PDWORD prev) {
    Init();
    (void)ign;
    int slot = FakeSlot(h);
    if (slot < 0) {
        Log("WlanRegisterNotification(src=0x%lx) passthrough", (unsigned long)src);
        return R.RegNotif ? R.RegNotif(h, src, ign, cb, ctx, rsv, prev) : ERROR_SERVICE_NOT_ACTIVE;
    }
    Log("WlanRegisterNotification(src=0x%lx, cb=%p) FAKE - ACM scan/connect events are emulated", (unsigned long)src, (void*)cb);
    bool unregister = (src == 0);              // WLAN_NOTIFICATION_SOURCE_NONE
    if (!unregister && !cb) return ERROR_INVALID_PARAMETER;
    EnterCriticalSection(&g_cs);
    if (!g_conn[slot].used) { LeaveCriticalSection(&g_cs); return ERROR_INVALID_HANDLE; }
    if (prev) *prev = g_conn[slot].src;
    g_conn[slot].src = unregister ? 0 : src;
    g_conn[slot].cb = unregister ? NULL : cb;
    g_conn[slot].ctx = unregister ? NULL : ctx;
    LeaveCriticalSection(&g_cs);
    if (unregister) {                          // wait out a callback that is running right now
        EnterCriticalSection(&g_dispatch_cs);
        LeaveCriticalSection(&g_dispatch_cs);
    }
    return ERROR_SUCCESS;
}

DWORD WINAPI Shim_WlanReasonCodeToString(DWORD code, DWORD cap, PWCHAR buf, PVOID rsv) {
    Init();
    if (R.Reason) return R.Reason(code, cap, buf, rsv);
    if (buf && cap) { _snwprintf(buf, cap, L"reason %lu", (unsigned long)code); buf[cap - 1] = 0; }
    return ERROR_SUCCESS;
}

DWORD WINAPI Shim_WlanSetInterface(HANDLE h, const GUID* g, WLAN_INTF_OPCODE op, DWORD sz, PVOID data, PVOID rsv) {
    Init();
    Log("WlanSetInterface(op=%d) %s", (int)op, IsFake(h, g) ? "FAKE" : "passthrough");
    if (IsFake(h, g)) return ERROR_SUCCESS;
    return R.SetIf ? R.SetIf(h, g, op, sz, data, rsv) : ERROR_SERVICE_NOT_ACTIVE;
}

DWORD WINAPI Shim_WlanGetInterfaceCapability(HANDLE h, const GUID* g, PVOID rsv, PWLAN_INTERFACE_CAPABILITY* pp) {
    Init();
    Log("WlanGetInterfaceCapability %s", IsFake(h, g) ? "FAKE (not supported)" : "passthrough");
    if (IsFake(h, g)) return ERROR_NOT_SUPPORTED;
    return R.Cap ? R.Cap(h, g, rsv, pp) : ERROR_SERVICE_NOT_ACTIVE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        InitializeCriticalSection(&g_cs);
        InitializeCriticalSection(&g_dispatch_cs);
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}

} // extern "C"

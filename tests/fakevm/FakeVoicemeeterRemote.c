// Fake Voicemeeter Remote API for tests under Wine (no real Voicemeeter needed).
// Simulates Voicemeeter Banana: Option.sr, Option.ASIOsr, Bus[0..2].device.name/.sr and
// Command.Restart. The state lives in C:\vmfake.ini so it survives AIMP restarts (like the real
// Voicemeeter, which runs as its own process); every call is logged to C:\vmfake.log.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define STATE_FILE "C:\\vmfake.ini"
#define LOG_FILE "C:\\vmfake.log"

static float optSr = 48000.0f, optAsioSr = 0.0f, engineSr = 48000.0f;
static wchar_t busName[3][128] = {L"FiiO KA11", L"Voicemeeter Input (VB-Audio Voicemeeter VAIO)", L""};
static long busType[3] = {3, 3, 0};  // WDM
static int dirty = 1, loggedIn = 0;

static void LogF(const char* fmt, ...) {
    FILE* f = fopen(LOG_FILE, "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list a;
    va_start(a, fmt);
    vfprintf(f, fmt, a);
    va_end(a);
    fputc('\n', f);
    fclose(f);
}

static void Load(void) {
    FILE* f = fopen(STATE_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char* v = eq + 1;
        int i = -1;
        char kind[16] = "";
        sscanf(line, "bus%d%15s", &i, kind);
        if (!strcmp(line, "sr")) optSr = (float)atof(v);
        else if (!strcmp(line, "asiosr")) optAsioSr = (float)atof(v);
        else if (!strcmp(line, "engine")) engineSr = (float)atof(v);
        else if (i >= 0 && i < 3 && !strcmp(kind, "name")) MultiByteToWideChar(CP_UTF8, 0, v, -1, busName[i], 128);
        else if (i >= 0 && i < 3 && !strcmp(kind, "type")) busType[i] = atol(v);
    }
    fclose(f);
}

static void Save(void) {
    FILE* f = fopen(STATE_FILE, "w");
    if (!f) return;
    fprintf(f, "sr=%.0f\nasiosr=%.0f\nengine=%.0f\n", optSr, optAsioSr, engineSr);
    for (int i = 0; i < 3; i++) {
        char n[512];
        WideCharToMultiByte(CP_UTF8, 0, busName[i], -1, n, sizeof n, NULL, NULL);
        fprintf(f, "bus%dname=%s\nbus%dtype=%ld\n", i, n, i, busType[i]);
    }
    fclose(f);
}

static int BusIndex(const char* p, const char* suffix) {
    int i;
    char s[64];
    if (sscanf(p, "Bus[%d].device.%63s", &i, s) == 2 && i >= 0 && i < 3 && !strcmp(s, suffix)) return i;
    return -1;
}

__declspec(dllexport) long __stdcall VBVMR_Login(void) {
    Load();
    loggedIn = 1;
    dirty = 1;
    LogF("Login");
    return 0;
}
__declspec(dllexport) long __stdcall VBVMR_Logout(void) {
    LogF("Logout (engine %.0f, Option.sr %.0f)", engineSr, optSr);
    loggedIn = 0;
    return 0;
}
__declspec(dllexport) long __stdcall VBVMR_IsParametersDirty(void) {
    int d = dirty;
    dirty = 0;
    return d;
}
__declspec(dllexport) long __stdcall VBVMR_GetVoicemeeterType(long* t) {
    if (!loggedIn) return -1;
    *t = sizeof(void*) == 8 ? 5 : 2;  // Banana (x64 variant on 64-bit)
    return 0;
}
__declspec(dllexport) long __stdcall VBVMR_GetVoicemeeterVersion(long* v) {
    *v = (2 << 24) | (1 << 16) | (1 << 8) | 9;
    return 0;
}
__declspec(dllexport) long __stdcall VBVMR_GetParameterFloat(char* p, float* v) {
    int i;
    Load();
    if (!strcmp(p, "Option.sr")) *v = optSr;
    else if (!strcmp(p, "Option.ASIOsr")) *v = optAsioSr;
    else if ((i = BusIndex(p, "sr")) >= 0) *v = busName[i][0] ? engineSr : 0.0f;
    else { LogF("GetFloat %s -> unknown", p); return -3; }
    return 0;
}
__declspec(dllexport) long __stdcall VBVMR_GetParameterStringW(char* p, unsigned short* out) {
    int i;
    Load();
    if ((i = BusIndex(p, "name")) >= 0) { lstrcpyW((wchar_t*)out, busName[i]); return 0; }
    LogF("GetString %s -> unknown", p);
    out[0] = 0;
    return -3;
}
__declspec(dllexport) long __stdcall VBVMR_SetParameterFloat(char* p, float v) {
    Load();
    LogF("SetFloat %s = %.0f", p, v);
    if (!strcmp(p, "Option.sr")) optSr = v;
    else if (!strcmp(p, "Option.ASIOsr")) optAsioSr = v;
    else if (!strcmp(p, "Command.Restart")) {
        LogF("Engine restart: %.0f -> %.0f", engineSr, optSr);
        engineSr = optSr;
    } else return -3;
    Save();
    dirty = 1;
    return 0;
}
__declspec(dllexport) long __stdcall VBVMR_SetParameterStringW(char* p, unsigned short* v) {
    int i;
    char n[256];
    Load();
    WideCharToMultiByte(CP_UTF8, 0, (wchar_t*)v, -1, n, sizeof n, NULL, NULL);
    LogF("SetString %s = %s", p, n);
    for (i = 0; i < 3; i++) {
        char k[32];
        snprintf(k, sizeof k, "Bus[%d].device.", i);
        if (!strncmp(p, k, strlen(k))) {
            lstrcpynW(busName[i], (wchar_t*)v, 128);
            const char* s = p + strlen(k);
            busType[i] = !strcmp(s, "mme") ? 1 : !strcmp(s, "wdm") ? 3 : !strcmp(s, "ks") ? 4 : 5;
            Save();
            dirty = 1;
            return 0;
        }
    }
    return -3;
}
__declspec(dllexport) long __stdcall VBVMR_Output_GetDeviceNumber(void) { return 2; }
__declspec(dllexport) long __stdcall VBVMR_Output_GetDeviceDescW(long i, long* type, unsigned short* name, unsigned short* hw) {
    static const wchar_t* names[2] = {L"FiiO KA11", L"Voicemeeter Input (VB-Audio Voicemeeter VAIO)"};
    if (i < 0 || i > 1) return -1;
    *type = 3;
    lstrcpyW((wchar_t*)name, names[i]);
    hw[0] = 0;
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) {
    (void)h; (void)p;
    if (r == DLL_PROCESS_ATTACH) LogF("Loaded (%d-bit)", (int)sizeof(void*) * 8);
    return TRUE;
}

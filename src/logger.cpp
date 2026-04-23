#include "logger.hpp"

#include <windows.h>
#include <psapi.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>

namespace wf {

static FILE* g_fp = nullptr;
static CRITICAL_SECTION g_cs;
static bool g_cs_ready = false;

void log_init(const char* exe_dir) {
    if (!g_cs_ready) {
        InitializeCriticalSection(&g_cs);
        g_cs_ready = true;
    }
    if (g_fp) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cod4-worldflip.log", exe_dir);
    g_fp = fopen(path, "w");
    if (g_fp) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_fp, "[%02d:%02d:%02d] cod4-worldflip log opened\n",
                st.wHour, st.wMinute, st.wSecond);
        fflush(g_fp);
    }
}

void log_shutdown() {
    if (g_cs_ready) {
        EnterCriticalSection(&g_cs);
    }
    if (g_fp) {
        fclose(g_fp);
        g_fp = nullptr;
    }
    if (g_cs_ready) {
        LeaveCriticalSection(&g_cs);
        DeleteCriticalSection(&g_cs);
        g_cs_ready = false;
    }
}

void logf(const char* fmt, ...) {
    if (!g_fp) return;
    EnterCriticalSection(&g_cs);
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_fp, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_fp, fmt, ap);
    va_end(ap);
    fputc('\n', g_fp);
    fflush(g_fp);
    LeaveCriticalSection(&g_cs);
}

void log_guid(const char* prefix, REFIID riid) {
    if (!g_fp) return;
    logf("%s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
         prefix,
         (unsigned long)riid.Data1, riid.Data2, riid.Data3,
         riid.Data4[0], riid.Data4[1], riid.Data4[2], riid.Data4[3],
         riid.Data4[4], riid.Data4[5], riid.Data4[6], riid.Data4[7]);
}

void log_loaded_modules() {
    if (!g_fp) return;
    HANDLE hproc = GetCurrentProcess();
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(hproc, mods, sizeof(mods), &needed)) return;
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i) {
        char path[MAX_PATH] = {0};
        if (!GetModuleFileNameA(mods[i], path, sizeof(path))) continue;
        // Lowercase basename for filtering.
        char base[MAX_PATH] = {0};
        const char* slash = std::strrchr(path, '\\');
        const char* b = slash ? slash + 1 : path;
        for (int j = 0; b[j] && j < (int)sizeof(base) - 1; ++j) {
            base[j] = (char)std::tolower((unsigned char)b[j]);
        }
        if (std::strstr(base, "d3d9") ||
            std::strstr(base, "dxgi") ||
            std::strstr(base, "reshade") ||
            std::strstr(base, "enbseries") ||
            std::strstr(base, "dinput8") ||
            std::strstr(base, "cod4x") ||
            std::strstr(base, "iw3xo")) {
            logf("module: %p %s", (void*)mods[i], path);
        }
    }
}

} // namespace wf

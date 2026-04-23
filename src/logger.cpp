#include "logger.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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

} // namespace wf

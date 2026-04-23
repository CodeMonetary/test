#include "config.hpp"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wf {

static char g_exe_dir[MAX_PATH] = {0};

static void compute_exe_dir() {
    if (g_exe_dir[0]) return;
    HMODULE host = GetModuleHandleA(nullptr);
    char path[MAX_PATH];
    GetModuleFileNameA(host, path, sizeof(path));
    // strip file name
    char* slash = strrchr(path, '\\');
    if (slash) *slash = '\0';
    strncpy(g_exe_dir, path, sizeof(g_exe_dir) - 1);
}

const char* config_exe_dir() {
    compute_exe_dir();
    return g_exe_dir;
}

static int parse_int(const char* s, int fallback) {
    if (!s || !*s) return fallback;
    // hex (0x..) or decimal
    if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        return (int)strtoul(s + 2, nullptr, 16);
    }
    return atoi(s);
}

void config_load(const char* exe_dir, Config* out) {
    char ini_path[MAX_PATH];
    snprintf(ini_path, sizeof(ini_path), "%s\\cod4-worldflip.ini", exe_dir);

    char buf[32];

    GetPrivateProfileStringA("worldflip", "enabled", "1",
                             buf, sizeof(buf), ini_path);
    out->enabled = (parse_int(buf, 1) != 0);

    GetPrivateProfileStringA("worldflip", "debug_banner", "0",
                             buf, sizeof(buf), ini_path);
    out->debug_banner = (parse_int(buf, 0) != 0);

    GetPrivateProfileStringA("worldflip", "log", "1",
                             buf, sizeof(buf), ini_path);
    out->log = (parse_int(buf, 1) != 0);

    GetPrivateProfileStringA("worldflip", "toggle_vk", "0x7A",
                             buf, sizeof(buf), ini_path);
    out->toggle_vk = parse_int(buf, 0x7A);
}

} // namespace wf

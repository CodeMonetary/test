#include "d3d9_wrapper.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <windows.h>
#include <d3d9.h>

namespace wf {

Config  g_cfg;

} // namespace wf

namespace {

using PFN_Direct3DCreate9 = IDirect3D9* (WINAPI*)(UINT);

HMODULE             g_real_d3d9      = nullptr;
PFN_Direct3DCreate9 g_real_create9   = nullptr;

static bool load_real_d3d9() {
    if (g_real_d3d9) return true;
    char sys[MAX_PATH];
    UINT n = GetSystemDirectoryA(sys, sizeof(sys));
    if (n == 0 || n >= sizeof(sys) - 12) return false;
    // Build "<SystemRoot>\System32\d3d9.dll"
    strcat_s(sys, sizeof(sys), "\\d3d9.dll");
    g_real_d3d9 = LoadLibraryA(sys);
    if (!g_real_d3d9) return false;
    g_real_create9 = reinterpret_cast<PFN_Direct3DCreate9>(
        GetProcAddress(g_real_d3d9, "Direct3DCreate9"));
    return g_real_create9 != nullptr;
}

} // namespace

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion) {
    if (!load_real_d3d9()) {
        MessageBoxA(nullptr,
            "cod4-worldflip: failed to load the real system d3d9.dll.\n"
            "Delete this proxy to restore default behavior.",
            "cod4-worldflip", MB_OK | MB_ICONERROR);
        return nullptr;
    }
    IDirect3D9* real = g_real_create9(SDKVersion);
    if (!real) {
        wf::logf("Direct3DCreate9 returned null (SDKVersion=%u)", SDKVersion);
        return nullptr;
    }
    wf::logf("Direct3DCreate9: wrapping IDirect3D9 (SDKVersion=%u)", SDKVersion);
    return new wf::D3D9Wrap(real);
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst;
    (void)reserved;
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hinst);
        const char* exe_dir = wf::config_exe_dir();
        wf::config_load(exe_dir, &wf::g_cfg);
        if (wf::g_cfg.log) {
            wf::log_init(exe_dir);
            wf::logf("=== cod4-worldflip v8 attached (mode=viewmodel-flip, cached-vp). dir=%s ===", exe_dir);
            wf::logf("config: enabled=%d debug_banner=%d log=%d toggle_vk=0x%02x",
                     (int)wf::g_cfg.enabled,
                     (int)wf::g_cfg.debug_banner,
                     (int)wf::g_cfg.log,
                     wf::g_cfg.toggle_vk);
            char self[MAX_PATH] = {0};
            GetModuleFileNameA(hinst, self, sizeof(self));
            wf::logf("self dll path: %s (hmodule=%p)", self, (void*)hinst);
            HMODULE h_named = GetModuleHandleA("d3d9.dll");
            wf::logf("GetModuleHandleA(\"d3d9.dll\") -> %p", (void*)h_named);
            wf::log_loaded_modules();
        }
        break;
    }
    case DLL_PROCESS_DETACH: {
        wf::logf("=== cod4-worldflip detached ===");
        wf::log_shutdown();
        if (g_real_d3d9) {
            FreeLibrary(g_real_d3d9);
            g_real_d3d9 = nullptr;
        }
        break;
    }
    default: break;
    }
    return TRUE;
}

// cod4mirror — proxy d3d9.dll for vanilla CoD4 1.7 MP (iw3mp.exe).
//
// Build target: 32-bit DLL named exactly "d3d9.dll", placed next to iw3mp.exe.
// Windows DLL search order will load this DLL first when iw3mp.exe imports
// d3d9.dll. We then load the real system32\d3d9.dll, forward every export to
// it, and wrap IDirect3D9 / IDirect3DDevice9 so future phases can hook
// rendering for mirror-viewmodel logic.
//
// Phase 1 (this file): pure passthrough. Goal = the game launches and looks
// identical to vanilla. No mirror logic active. Once verified, later phases
// add hooks in d3d9_device.cpp and a separate engine-hook module.
#include <Windows.h>
#include <d3d9.h>
#include "d3d9_object.h"

namespace
{
	HMODULE g_real_d3d9 = nullptr;

	// Resolve a symbol from the real d3d9.dll (cached LoadLibrary on first
	// call). Returns nullptr if symbol/library missing.
	FARPROC resolve(const char* name)
	{
		if (!g_real_d3d9)
		{
			char path[MAX_PATH];
			const UINT n = GetSystemDirectoryA(path, MAX_PATH);
			if (n == 0 || n >= MAX_PATH - 11) return nullptr;
			lstrcatA(path, "\\d3d9.dll");
			g_real_d3d9 = LoadLibraryA(path);
		}
		return g_real_d3d9 ? GetProcAddress(g_real_d3d9, name) : nullptr;
	}
}

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD reason, LPVOID /*lpReserved*/)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		// preload real d3d9 so the first export call has no latency hit.
		resolve("Direct3DCreate9");
		break;
	case DLL_PROCESS_DETACH:
		if (g_real_d3d9) { FreeLibrary(g_real_d3d9); g_real_d3d9 = nullptr; }
		break;
	default: break;
	}
	return TRUE;
}

// ---------------------------------------------------------------------------
// d3d9.dll exports. Names must match windows d3d9.dll exactly (see d3d9.def).
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport)
IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
	using fn_t = IDirect3D9*(WINAPI*)(UINT);
	auto fn = reinterpret_cast<fn_t>(resolve("Direct3DCreate9"));
	if (!fn) return nullptr;
	IDirect3D9* real = fn(SDKVersion);
	if (!real) return nullptr;
	return new cod4mirror::D3D9Object(real);
}

extern "C" __declspec(dllexport)
HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
	// CoD4 1.7 doesn't use the Ex path; passthrough without wrapping is fine.
	using fn_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
	auto fn = reinterpret_cast<fn_t>(resolve("Direct3DCreate9Ex"));
	if (!fn) return E_NOTIMPL;
	return fn(SDKVersion, ppD3D);
}

// Pure passthrough exports. None of these are called on hot paths so
// per-call GetProcAddress is fine. Keeps the file tiny.
#define PASSTHROUGH(ret, name, args, call_args)        \
	extern "C" __declspec(dllexport) ret WINAPI name args \
	{                                                  \
		using fn_t = ret(WINAPI*)args;                 \
		auto fn = reinterpret_cast<fn_t>(resolve(#name)); \
		return fn ? fn call_args : (ret)0;             \
	}

PASSTHROUGH(int,   D3DPERF_BeginEvent,                     (D3DCOLOR c, LPCWSTR n),         (c, n))
PASSTHROUGH(int,   D3DPERF_EndEvent,                       (),                              ())
PASSTHROUGH(DWORD, D3DPERF_GetStatus,                      (),                              ())
PASSTHROUGH(BOOL,  D3DPERF_QueryRepeatFrame,               (),                              ())
PASSTHROUGH(void,  D3DPERF_SetMarker,                      (D3DCOLOR c, LPCWSTR n),         (c, n))
PASSTHROUGH(void,  D3DPERF_SetOptions,                     (DWORD o),                       (o))
PASSTHROUGH(void,  D3DPERF_SetRegion,                      (D3DCOLOR c, LPCWSTR n),         (c, n))
PASSTHROUGH(int,   DebugSetLevel,                          (int l),                         (l))
PASSTHROUGH(int,   DebugSetMute,                           (),                              ())
PASSTHROUGH(int,   Direct3D9EnableMaximizedWindowedModeShim, (UINT m),                      (m))

// Direct3DShaderValidatorCreate9 takes no args, returns IUnknown* on most
// platforms. Some toolchains type it as void*; treat as void* for simplicity.
extern "C" __declspec(dllexport) void* WINAPI Direct3DShaderValidatorCreate9()
{
	using fn_t = void*(WINAPI*)();
	auto fn = reinterpret_cast<fn_t>(resolve("Direct3DShaderValidatorCreate9"));
	return fn ? fn() : nullptr;
}

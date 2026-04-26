// cod4mirror — proxy d3d9.dll for vanilla CoD4 1.7 MP (iw3mp.exe).
//
// Build target: 32-bit DLL named exactly "d3d9.dll", placed next to iw3mp.exe.
// Windows DLL search order will load this DLL first when iw3mp.exe imports
// d3d9.dll. We then load the real system32\d3d9.dll, forward every export to
// it, and wrap IDirect3D9 / IDirect3DDevice9 so future phases can hook
// rendering for mirror-viewmodel logic.
//
// Phase 1 (this file): pure passthrough. Goal = the game launches and looks
// identical to vanilla. No mirror logic active.
//
// IMPORTANT: <d3d9.h> declares Direct3DCreate9 / D3DPERF_* / etc. with
// __declspec(dllimport). We CANNOT redeclare them with __declspec(dllexport)
// in the same TU (C2375). Instead we expose them under internal C++ names
// (proxy_*) and let d3d9.def map the public d3d9.dll export names to those
// internal symbols.
#include <Windows.h>
#include <d3d9.h>
#include "d3d9_object.h"

namespace
{
	HMODULE g_self      = nullptr; // our own DLL — used to find sibling files
	HMODULE g_real_d3d9 = nullptr;

	// Build an absolute path "<our DLL folder>\<name>". Returns false if our
	// own module path isn't available or doesn't fit.
	bool sibling_path(const char* name, char* out, size_t cap)
	{
		if (!g_self) return false;
		char dir[MAX_PATH];
		const DWORD n = GetModuleFileNameA(g_self, dir, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) return false;
		// strip the filename portion
		for (DWORD i = n; i > 0; --i)
		{
			if (dir[i - 1] == '\\' || dir[i - 1] == '/') { dir[i] = 0; break; }
			dir[i - 1] = 0;
		}
		const size_t dl = lstrlenA(dir);
		const size_t nl = lstrlenA(name);
		if (dl + nl + 1 > cap) return false;
		lstrcpyA(out, dir);
		lstrcatA(out, name);
		return true;
	}

	// Resolve a symbol from the real d3d9.dll (cached LoadLibrary on first
	// call). Chain-loading order:
	//   1) <game folder>\d3d9_chain.dll   (e.g. ReShade renamed to chain)
	//   2) %SystemRoot%\System32\d3d9.dll (vanilla fallback)
	// This lets the user stack our proxy on top of ReShade (or any other
	// d3d9 wrapper) without renaming exports. Detected via file-existence
	// check so absence of the chain file is silent.
	FARPROC resolve(const char* name)
	{
		if (!g_real_d3d9)
		{
			char chain[MAX_PATH];
			if (sibling_path("d3d9_chain.dll", chain, MAX_PATH) &&
				GetFileAttributesA(chain) != INVALID_FILE_ATTRIBUTES)
			{
				// Use absolute path so Windows doesn't search cwd / our dir
				// recursively (which would re-load us and infinite-loop).
				g_real_d3d9 = LoadLibraryA(chain);
			}
			if (!g_real_d3d9)
			{
				char sys[MAX_PATH];
				const UINT n = GetSystemDirectoryA(sys, MAX_PATH);
				if (n == 0 || n >= MAX_PATH - 11) return nullptr;
				lstrcatA(sys, "\\d3d9.dll");
				g_real_d3d9 = LoadLibraryA(sys);
			}
		}
		return g_real_d3d9 ? GetProcAddress(g_real_d3d9, name) : nullptr;
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*lpReserved*/)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		g_self = hModule;
		DisableThreadLibraryCalls(hModule);
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
// Internal proxy implementations. d3d9.def exports them under the public
// d3d9.dll names (Direct3DCreate9, D3DPERF_*, ...). NOT marked dllexport
// because the .def file does that, and using dllexport here would conflict
// with d3d9.h's dllimport-prefixed declarations.
// ---------------------------------------------------------------------------

extern "C" IDirect3D9* WINAPI proxy_Direct3DCreate9(UINT SDKVersion)
{
	using fn_t = IDirect3D9*(WINAPI*)(UINT);
	auto fn = reinterpret_cast<fn_t>(resolve("Direct3DCreate9"));
	if (!fn) return nullptr;
	IDirect3D9* real = fn(SDKVersion);
	if (!real) return nullptr;
	return new cod4mirror::D3D9Object(real);
}

extern "C" HRESULT WINAPI proxy_Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
	// CoD4 1.7 doesn't use the Ex path; passthrough without wrapping is fine.
	using fn_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
	auto fn = reinterpret_cast<fn_t>(resolve("Direct3DCreate9Ex"));
	if (!fn) return E_NOTIMPL;
	return fn(SDKVersion, ppD3D);
}

extern "C" void* WINAPI proxy_Direct3DShaderValidatorCreate9()
{
	using fn_t = void*(WINAPI*)();
	auto fn = reinterpret_cast<fn_t>(resolve("Direct3DShaderValidatorCreate9"));
	return fn ? fn() : nullptr;
}

// Pure passthrough exports. Macro name avoids collision with wingdi.h's
// PASSTHROUGH define.
#define D3D9_PROXY(ret, name, args, call_args)         \
	extern "C" ret WINAPI proxy_##name args            \
	{                                                  \
		using fn_t = ret(WINAPI*)args;                 \
		auto fn = reinterpret_cast<fn_t>(resolve(#name)); \
		return fn ? fn call_args : (ret)0;             \
	}

D3D9_PROXY(int,   D3DPERF_BeginEvent,                     (D3DCOLOR c, LPCWSTR n),         (c, n))
D3D9_PROXY(int,   D3DPERF_EndEvent,                       (),                              ())
D3D9_PROXY(DWORD, D3DPERF_GetStatus,                      (),                              ())
D3D9_PROXY(BOOL,  D3DPERF_QueryRepeatFrame,               (),                              ())
D3D9_PROXY(void,  D3DPERF_SetMarker,                      (D3DCOLOR c, LPCWSTR n),         (c, n))
D3D9_PROXY(void,  D3DPERF_SetOptions,                     (DWORD o),                       (o))
D3D9_PROXY(void,  D3DPERF_SetRegion,                      (D3DCOLOR c, LPCWSTR n),         (c, n))
D3D9_PROXY(int,   DebugSetLevel,                          (int l),                         (l))
D3D9_PROXY(int,   DebugSetMute,                           (),                              ())
D3D9_PROXY(int,   Direct3D9EnableMaximizedWindowedModeShim, (UINT m),                      (m))

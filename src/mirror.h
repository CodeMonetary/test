// cod4mirror — mirror viewmodel state + entry points called from D3D9Device
// hooks. Phase 2a: dvar registration only; Phase 3 will fill in the mirror
// pipeline in mirror.cpp.
#pragma once
#include <Windows.h>
#include <d3d9.h>

namespace cod4mirror::mirror
{
	// Called once per frame from D3D9Device::Present. Runs one-shot
	// initialization (engine dvar registration) on the first frame, when the
	// engine is fully up and Dvar_RegisterInt is safe to call.
	void on_present(IDirect3DDevice9* dev);

	// Hook entry points wired from D3D9Device methods. Phase 2a stubs are
	// no-ops; Phase 3 will implement the RTT mirror, tonemap inject, and
	// fullscreen flip.
	void on_set_vertex_shader_constant_f(IDirect3DDevice9* dev, UINT start, const float* data, UINT count);
	void on_set_pixel_shader_constant_f (IDirect3DDevice9* dev, UINT start, const float* data, UINT count);
	void on_after_draw                  (IDirect3DDevice9* dev);
	void on_end_scene                   (IDirect3DDevice9* dev);

	// Called from D3D9Device::Reset BEFORE the underlying Reset. Must release
	// every POOL_DEFAULT resource we hold (off-screen RT textures, depth
	// stencil, flip texture) or Reset will fail with D3DERR_DEVICELOST.
	void on_device_reset();
}

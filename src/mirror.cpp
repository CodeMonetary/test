// cod4mirror — Phase 2a stub. On first Present we register all the user-
// facing dvars by calling vanilla iw3mp.exe's Dvar_RegisterInt at 0x56C600.
// Phase 3 will add the actual RTT mirror pipeline.
#include "mirror.h"
#include "engine.h"
#include <cstdio>

namespace cod4mirror::mirror
{
	namespace
	{
		bool g_init_done = false;

		// dvar handles. Phase 3 will read .current.integer / .current.value
		// from these to drive runtime behaviour. Phase 2a registers them so
		// the user can confirm the DLL is talking to the engine.
		engine::dvar_s* d_rtt                 = nullptr;
		engine::dvar_s* d_tonemap_inject      = nullptr;
		engine::dvar_s* d_early_composite     = nullptr;
		engine::dvar_s* d_composite_srgb      = nullptr;
		engine::dvar_s* d_rtt_blend           = nullptr;
		engine::dvar_s* d_full_mirror         = nullptr;
		engine::dvar_s* d_mirror_fx           = nullptr;
		engine::dvar_s* d_mirror_fx_axis      = nullptr;
		engine::dvar_s* d_mirror_fx_dist      = nullptr;

		void debug_log(const char* msg)
		{
			OutputDebugStringA(msg);
		}

		void register_all()
		{
			using namespace engine;

			d_rtt = Dvar_RegisterInt("r_mirrorViewmodel_rtt",
				"cod4mirror: master switch for RTT mirror viewmodel. 0=off, 1=on.",
				0, 0, 1);

			d_tonemap_inject = Dvar_RegisterInt("r_mirrorViewmodel_rttTonemapInject",
				"cod4mirror: composite mirrored gun into engine's tonemap source "
				"(correct lighting). 0=legacy bb composite, 1=tonemap inject (default).",
				1, 0, 1);

			d_early_composite = Dvar_RegisterInt("r_mirrorViewmodel_rttEarlyComposite",
				"cod4mirror: 0=composite at EndScene (gun covers HUD); "
				"1=composite at PSCF c7 fingerprint (HUD on top of gun, default); "
				"2=both (diagnostic).",
				1, 0, 2);

			d_composite_srgb = Dvar_RegisterInt("r_mirrorViewmodel_compositeSrgb",
				"cod4mirror: sRGB encoding for composite. 0=raw linear, "
				"1=linear->sRGB write (default), 2=sRGB sample only, 3=both.",
				1, 0, 3);

			d_rtt_blend = Dvar_RegisterInt("r_mirrorViewmodel_rttBlend",
				"cod4mirror: composite blend mode. "
				"0=SRCALPHA/INVSRCALPHA, 1=+ALPHATEST, "
				"2=ONE/ONE additive (default), 3=+ALPHATEST.",
				2, 0, 3);

			d_full_mirror = Dvar_RegisterInt("r_fullMirror",
				"cod4mirror: full-screen mirror for montage. "
				"0=off, 1=mirror world+gun before HUD (HUD intact), "
				"2=mirror everything including HUD.",
				0, 0, 2);

			d_mirror_fx = Dvar_RegisterInt("r_mirrorViewmodel_mirrorFx",
				"cod4mirror: mirror first-person FX (muzzleflash, brass) — "
				"requires Phase 4 hook (not yet wired in this build).",
				0, 0, 1);

			d_mirror_fx_axis = Dvar_RegisterInt("r_mirrorViewmodel_mirrorFxAxis",
				"cod4mirror: FX axis mirror mode. 0=origin only, "
				"1=full reflect (CRASH), 2=RH-mirror (recommended).",
				2, 0, 2);

			d_mirror_fx_dist = Dvar_RegisterFloat("r_mirrorViewmodel_mirrorFxDist",
				"cod4mirror: FX mirror distance threshold (world units).",
				64.0f, 0.0f, 4096.0f);

			char buf[256];
			std::snprintf(buf, sizeof(buf),
				"[cod4mirror] dvars registered: rtt=%p, fullMirror=%p\n",
				(void*)d_rtt, (void*)d_full_mirror);
			debug_log(buf);
		}
	}

	void on_present(IDirect3DDevice9* /*dev*/)
	{
		if (g_init_done) return;
		g_init_done = true;

		// First Present means the engine is fully up — Dvar_RegisterInt at
		// 0x56C600 is safe to call. Wrap in __try so a bad address doesn't
		// kill the game on Phase 2a verification.
		__try
		{
			register_all();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			debug_log("[cod4mirror] dvar registration crashed — address mismatch?\n");
		}
	}

	// Phase 2a stubs — real implementations land in Phase 3.
	void on_set_vertex_shader_constant_f(IDirect3DDevice9*, UINT, const float*, UINT) {}
	void on_set_pixel_shader_constant_f (IDirect3DDevice9*, UINT, const float*, UINT) {}
	void on_after_draw                  (IDirect3DDevice9*) {}
	void on_end_scene                   (IDirect3DDevice9*) {}
}

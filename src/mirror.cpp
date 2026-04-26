// cod4mirror — RTT mirror viewmodel pipeline ported from iw3xo v15-v32.
//
// Pipeline (matches iw3xo behaviour for r_mirrorViewmodel_rtt 1):
//
//   * VSCF c0..c3 with c2[3] in [-0.5, -0.02]   = depth-hack proj (gun pass start)
//     -> begin_segment: bind off-screen RT (color+depth), clear once per frame
//   * VSCF c0..c3 with c2[3] not in dhp range   = world / 2D proj
//     -> end_segment: restore engine RT (off-screen accumulates across multiple
//        gun segments without re-clearing so the lit pass z-tests against the
//        z-prefill pass)
//   * PSCF c7 with shape (c70=c71=c72 in [-0.2, 0), c73 in (1, 5))
//     = engine's final tonemap/output. We either:
//     - composite the off-screen mirror into the engine's tonemap SOURCE
//       texture (r_mirrorViewmodel_rttTonemapInject 1, default), so the
//       gun gets the same film/grade as the world; or
//     - set g_pending_early_composite so the final composite fires AFTER
//       the next draw (the engine's tonemap-output write).
//   * EndScene: safety-net composite if neither path hit; r_fullMirror==2
//     does an unconditional fullscreen horizontal flip.
//
// The "mirror" effect comes from the composite quad's UVs being horizontally
// inverted: the off-screen target was rendered with the engine's normal
// projection (not flipped), so flipping the UVs at composite time produces
// the visual mirror. Vertex tangents and culling stay correct internally,
// which avoids the lighting/cull artifacts the legacy matrix-flip path had.
#include "mirror.h"
#include "engine.h"
#include "fx_mirror.h"
#include "logger.h"
#include <cstdio>

namespace cod4mirror::mirror
{
	namespace
	{
		// ---- dvar handles -------------------------------------------------
		engine::dvar_s* d_rtt                 = nullptr;
		engine::dvar_s* d_tonemap_inject      = nullptr;
		engine::dvar_s* d_early_composite     = nullptr;
		engine::dvar_s* d_composite_srgb      = nullptr;
		engine::dvar_s* d_rtt_blend           = nullptr;
		engine::dvar_s* d_full_mirror         = nullptr;
		engine::dvar_s* d_mirror_fx           = nullptr;
		engine::dvar_s* d_mirror_fx_axis      = nullptr;
		engine::dvar_s* d_mirror_fx_dist      = nullptr;
		engine::dvar_s* d_mirror_fx_axis_idx  = nullptr;
		engine::dvar_s* d_mirror_fx_log       = nullptr;
		engine::dvar_s* d_depth_fix           = nullptr;

		bool g_init_done = false;

		// ---- RTT state ----------------------------------------------------
		IDirect3DTexture9* g_tex          = nullptr;
		IDirect3DSurface9* g_color        = nullptr;
		IDirect3DSurface9* g_depth        = nullptr;
		IDirect3DSurface9* g_depth_backup = nullptr; // snapshot of main depth before gun pass
		IDirect3DSurface9* g_saved_color  = nullptr;
		IDirect3DSurface9* g_saved_depth  = nullptr;
		int  g_w = 0;
		int  g_h = 0;
		bool g_pass_active                = false;
		bool g_in_segment                 = false;
		bool g_pending_early_composite    = false;
		bool g_have_depth_snapshot        = false; // valid this frame
		bool g_depth_backup_unsupported   = false; // sticky disable if StretchRect fails

		IDirect3DTexture9* g_flip_tex     = nullptr;
		IDirect3DSurface9* g_flip_surf    = nullptr;
		int  g_flip_w = 0;
		int  g_flip_h = 0;
		bool g_pending_fullmirror_flip    = false;

		// frame counters used to throttle hot-path logging.
		unsigned g_frame                = 0;
		unsigned g_vscf_calls           = 0;
		unsigned g_pscf_calls           = 0;
		unsigned g_vscf_dhp_hits        = 0;
		unsigned g_pscf_tonemap_hits    = 0;
		bool     g_logged_first_present = false;
		bool     g_logged_first_endscene = false;
		bool     g_logged_first_vscf    = false;
		bool     g_logged_first_pscf    = false;
		float    g_min_c23 = 1e9f, g_max_c23 = -1e9f;

		// Lazy init. Some IW3 dispatch paths route Present() around the d3d9
		// wrapper (confirmed by iw3xo: 0 Present hits vs many BeginScene), so
		// we cannot rely on Present being called at all. Instead, EVERY mirror
		// entry point calls this on first invocation — the first hook to fire
		// (PSCF, VSCF, EndScene, or Present) will trigger registration.
		void register_all();
		void lazy_init(const char* who)
		{
			if (g_init_done) return;
			g_init_done = true;
			log::reset();
			log::line("[init] first hook fired: %s — initializing dvars", who);
			__try { register_all(); }
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				log::line("[register] !!! SEH exception during Dvar_Register "
					"call — address 0x%08X is wrong",
					(unsigned)engine::kAddr_Dvar_Register);
			}
		}

		// ---- dvar registration -------------------------------------------
		void register_all()
		{
			using namespace engine;
			log::line("[register] calling Dvar_Register at 0x%08X ...",
				(unsigned)kAddr_Dvar_Register);
			d_rtt              = Dvar_RegisterInt("r_mirrorViewmodel_rtt",
				"cod4mirror: master switch for RTT mirror viewmodel. 0=off, 1=on.", 0, 0, 1);
			d_tonemap_inject   = Dvar_RegisterInt("r_mirrorViewmodel_rttTonemapInject",
				"cod4mirror: composite into tonemap source (correct grade). 0=bb, 1=tonemap.",
				1, 0, 1);
			d_early_composite  = Dvar_RegisterInt("r_mirrorViewmodel_rttEarlyComposite",
				"cod4mirror: 0=composite at EndScene (gun over HUD); "
				"1=composite at PSCF c7 (HUD over gun, default); 2=both.", 1, 0, 2);
			d_composite_srgb   = Dvar_RegisterInt("r_mirrorViewmodel_compositeSrgb",
				"cod4mirror: 0=raw, 1=sRGB write (default), 2=sRGB read, 3=both.", 1, 0, 3);
			d_rtt_blend        = Dvar_RegisterInt("r_mirrorViewmodel_rttBlend",
				"cod4mirror: blend mode. 0=alpha (default), 1=alpha+test, 2=add, 3=add+test.",
				0, 0, 3);
			d_full_mirror      = Dvar_RegisterInt("r_fullMirror",
				"cod4mirror: 0=off, 1=mirror world+gun before HUD, 2=mirror everything.",
				0, 0, 2);
			d_mirror_fx        = Dvar_RegisterInt("r_mirrorViewmodel_mirrorFx",
				"cod4mirror: mirror first-person FX (Phase 4, not yet wired).", 0, 0, 1);
			d_mirror_fx_axis   = Dvar_RegisterInt("r_mirrorViewmodel_mirrorFxAxis",
				"cod4mirror: FX axis mode. 0=origin, 1=full-reflect, 2=RH-mirror.", 2, 0, 2);
			d_mirror_fx_dist   = Dvar_RegisterFloat("r_mirrorViewmodel_mirrorFxDist",
				"cod4mirror: FX mirror distance threshold (units).", 64.0f, 0.0f, 4096.0f);
			d_mirror_fx_axis_idx = Dvar_RegisterInt("r_mirrorViewmodel_mirrorFxAxisIdx",
				"cod4mirror: which view-axis row defines the FX mirror plane normal. "
				"0=forward, 1=side (default), 2=up.", 1, 0, 2);
			d_mirror_fx_log    = Dvar_RegisterInt("r_mirrorViewmodel_mirrorFxLog",
				"cod4mirror: log up to N tag-mirror reflections. Decrements on each "
				"hit; set to a positive int to dump a one-shot trace.", 0, 0, 1024);
			d_depth_fix        = Dvar_RegisterInt("r_mirrorViewmodel_depthFix",
				"cod4mirror: rewrite main depth at gun pixels for post-process AO "
				"(e.g. ReShade MXAO). 0=off; 1=both passes (z=1.0 right-far + "
				"z=0.0 left-near) — leaves a depth discontinuity that MXAO turns "
				"into a silhouette; 2=pass 2 only (default, left-near stamp only, "
				"keeps right-side gun depth so MXAO sees a continuous occluder); "
				"3=both passes with z=0.9999.",
				2, 0, 3);

			log::line("[register] dvar handles: rtt=%p tonemap=%p early=%p "
				"srgb=%p blend=%p full=%p fx=%p fxAxis=%p fxDist=%p",
				(void*)d_rtt, (void*)d_tonemap_inject, (void*)d_early_composite,
				(void*)d_composite_srgb, (void*)d_rtt_blend, (void*)d_full_mirror,
				(void*)d_mirror_fx, (void*)d_mirror_fx_axis, (void*)d_mirror_fx_dist);
			if (d_rtt)
			{
				const int v = engine::read_dvar_int(d_rtt);
				log::line("[register] read-back r_mirrorViewmodel_rtt = %d "
					"(should be 0 on first launch)", v);
			}
			else
			{
				log::line("[register] !!! d_rtt is NULL — registration FAILED. "
					"Address 0x%08X is wrong for this iw3mp.exe build.",
					(unsigned)kAddr_Dvar_Register);
			}

			// Phase 4: install the CG_DObjGetWorldBoneMatrix inline hook
			// now that the engine is fully up. The hook is a no-op at
			// runtime while r_mirrorViewmodel_mirrorFx is 0 (default), so
			// installing it unconditionally has no visible effect for
			// users who don't opt in.
			__try { fx_mirror::install(); }
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				log::line("[fx_mirror] !!! SEH during install — hook NOT active.");
			}
		}
	}

	// dvar accessors exposed to fx_mirror.cpp (extern "C" linkage so the
	// translation unit doesn't need to know about the cod4mirror::mirror
	// namespace, and so the names match what fx_mirror.cpp forward-declares).
	extern "C" int   fx_mirror_enabled()    { return engine::read_dvar_int(d_mirror_fx); }
	extern "C" int   fx_mirror_axis_mode()  { return engine::read_dvar_int(d_mirror_fx_axis); }
	extern "C" float fx_mirror_dist()       { return d_mirror_fx_dist ? engine::read_dvar_float(d_mirror_fx_dist) : 64.0f; }
	extern "C" int   fx_mirror_log_left()   { return engine::read_dvar_int(d_mirror_fx_log); }
	extern "C" int   fx_mirror_axis_idx()   { return engine::read_dvar_int(d_mirror_fx_axis_idx); }
	extern "C" int   fx_mirror_active()
	{
		const int rtt  = engine::read_dvar_int(d_rtt);
		const int full = engine::read_dvar_int(d_full_mirror);
		return (rtt || full) ? 1 : 0;
	}
	extern "C" void  fx_mirror_log_dec()
	{
		if (!d_mirror_fx_log) return;
		const int v = engine::read_dvar_int(d_mirror_fx_log);
		if (v > 0)
		{
			// dvar struct: current.integer at offset 12 (matches engine layout).
			*reinterpret_cast<int*>(reinterpret_cast<std::uint8_t*>(d_mirror_fx_log) + 12) = v - 1;
		}
	}

	namespace
	{
		// ---- RTT off-screen target management -----------------------------
		void release_targets()
		{
			if (g_color)        { g_color->Release();        g_color        = nullptr; }
			if (g_depth)        { g_depth->Release();        g_depth        = nullptr; }
			if (g_depth_backup) { g_depth_backup->Release(); g_depth_backup = nullptr; }
			if (g_tex)          { g_tex->Release();          g_tex          = nullptr; }
			g_w = g_h = 0;
			g_have_depth_snapshot = false;
		}

		bool ensure_targets(IDirect3DDevice9* dev)
		{
			IDirect3DSurface9* bb = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
			D3DSURFACE_DESC bd; bb->GetDesc(&bd); bb->Release();
			if (g_tex && (int)bd.Width == g_w && (int)bd.Height == g_h) return true;
			release_targets();
			if (FAILED(dev->CreateTexture(bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET,
				D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_tex, nullptr))) return false;
			if (FAILED(g_tex->GetSurfaceLevel(0, &g_color))) { release_targets(); return false; }
			if (FAILED(dev->CreateDepthStencilSurface(bd.Width, bd.Height, D3DFMT_D24S8,
				D3DMULTISAMPLE_NONE, 0, TRUE, &g_depth, nullptr))) { release_targets(); return false; }

			// NOTE: g_depth_backup is created lazily in begin_segment,
			// AFTER we know the engine's main depth surface description
			// (format, multisample type, multisample quality). Creating
			// it here from the back-buffer desc was producing surfaces
			// that didn't match the engine's depth and StretchRect failed
			// with D3DERR_INVALIDCALL.

			g_w = (int)bd.Width;
			g_h = (int)bd.Height;
			return true;
		}

		// Create the depth backup surface to EXACTLY match the description
		// of the engine's currently-bound main depth-stencil. StretchRect
		// between depth surfaces requires identical format AND multisample
		// AND multisample-quality on most drivers — guessing wrong fails
		// with D3DERR_INVALIDCALL.
		void ensure_depth_backup(IDirect3DDevice9* dev, IDirect3DSurface9* main_dsv)
		{
			if (g_depth_backup_unsupported || !main_dsv) return;
			D3DSURFACE_DESC d{};
			if (FAILED(main_dsv->GetDesc(&d))) return;
			if (g_depth_backup)
			{
				D3DSURFACE_DESC b{};
				if (SUCCEEDED(g_depth_backup->GetDesc(&b))
					&& b.Format == d.Format
					&& b.Width == d.Width
					&& b.Height == d.Height
					&& b.MultiSampleType == d.MultiSampleType
					&& b.MultiSampleQuality == d.MultiSampleQuality)
				{
					return; // backup matches, nothing to do
				}
				g_depth_backup->Release();
				g_depth_backup = nullptr;
			}
			HRESULT hr = dev->CreateDepthStencilSurface(d.Width, d.Height, d.Format,
				d.MultiSampleType, d.MultiSampleQuality, TRUE, &g_depth_backup, nullptr);
			if (FAILED(hr))
			{
				log::line("[depth_backup] CreateDepthStencilSurface failed for "
					"engine spec (fmt=%u w=%u h=%u msaa=%u qual=%u hr=0x%08X); "
					"snapshot disabled.",
					(unsigned)d.Format, (unsigned)d.Width, (unsigned)d.Height,
					(unsigned)d.MultiSampleType, (unsigned)d.MultiSampleQuality,
					(unsigned)hr);
				g_depth_backup            = nullptr;
				g_depth_backup_unsupported = true;
				return;
			}
			log::line("[depth_backup] backup created to match engine: "
				"fmt=%u w=%u h=%u msaa=%u qual=%u",
				(unsigned)d.Format, (unsigned)d.Width, (unsigned)d.Height,
				(unsigned)d.MultiSampleType, (unsigned)d.MultiSampleQuality);
		}

		void begin_segment(IDirect3DDevice9* dev)
		{
			if (g_in_segment) return;
			if (!ensure_targets(dev)) return;
			if (FAILED(dev->GetRenderTarget(0, &g_saved_color))) { g_saved_color = nullptr; return; }
			if (FAILED(dev->GetDepthStencilSurface(&g_saved_depth))) { g_saved_depth = nullptr; }

			// On the FIRST gun segment of the frame, snapshot the engine's
			// main depth-stencil into our backup surface. At this point the
			// engine has rendered the world but not yet the viewmodel, so
			// the snapshot represents "depth at every pixel as if the gun
			// did not exist". We restore from this in apply_main_depth_fix
			// so the right-side gun-shape AO ghost (caused by leaked gun
			// depth that escaped our redirect) is wiped out.
			if (!g_pass_active && g_saved_depth)
			{
				ensure_depth_backup(dev, g_saved_depth);
				if (g_depth_backup && !g_depth_backup_unsupported)
				{
					HRESULT hr = dev->StretchRect(g_saved_depth, nullptr,
						g_depth_backup, nullptr, D3DTEXF_NONE);
					if (SUCCEEDED(hr))
					{
						g_have_depth_snapshot = true;
					}
					else
					{
						static bool s_logged = false;
						if (!s_logged)
						{
							s_logged = true;
							log::line("[depth_backup] StretchRect main->backup failed "
								"even with matching desc (hr=0x%08X); driver "
								"refuses depth-StretchRect, snapshot disabled.",
								(unsigned)hr);
						}
						g_depth_backup_unsupported = true;
						g_have_depth_snapshot      = false;
					}
				}
			}

			dev->SetRenderTarget(0, g_color);
			dev->SetDepthStencilSurface(g_depth);
			if (!g_pass_active)
			{
				dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
					0x00000000, 1.0f, 0);
				g_pass_active = true;
			}
			g_in_segment = true;
		}

		void end_segment(IDirect3DDevice9* dev)
		{
			if (!g_in_segment) return;
			g_in_segment = false;
			if (g_saved_color) { dev->SetRenderTarget(0, g_saved_color); g_saved_color->Release(); g_saved_color = nullptr; }
			if (g_saved_depth) { dev->SetDepthStencilSurface(g_saved_depth); g_saved_depth->Release(); g_saved_depth = nullptr; }
			else                 dev->SetDepthStencilSurface(nullptr);
		}

		// Rewrite the engine's MAIN depth buffer at the pixels covered by
		// the mirrored gun, so post-process AO (e.g. ReShade MXAO) doesn't
		// sample the wall behind the gun and bleed shadow through it.
		//
		// Two passes, both with COLORWRITE=0, ZWRITE=TRUE, ZFUNC=ALWAYS,
		// alpha-test on the RTT alpha:
		//   1. UV unflipped, z=1 (far): clears any "ghost gun" depth left
		//      on the right side of the screen by an engine z-prefill we
		//      didn't redirect into the off-screen target. MXAO then sees
		//      the wall/world behind, not the invisible gun shape.
		//   2. UV horizontally flipped, z=0 (near): writes near depth at
		//      the mirrored gun's pixels on the LEFT side. MXAO sees a
		//      close object and skips the far-wall AO contribution that
		//      was bleeding through the composite.
		//
		// Caller invariants: engine main render-target + main depth-stencil
		// MUST be bound. We don't touch the render target — only depth.
		//
		// mode argument matches r_mirrorViewmodel_depthFix:
		//   1 = both passes, z=1.0 for pass 1
		//   2 = pass 2 only (skip right-ghost clear)
		//   3 = both passes, z=0.9999 for pass 1 (in case driver clips
		//       at exactly the far plane on D3DCMP_ALWAYS writes)
		void apply_main_depth_fix(IDirect3DDevice9* dev, int mode)
		{
			if (mode <= 0) return;
			if (!g_pass_active || !g_tex || g_w <= 0 || g_h <= 0) return;
			const float far_z  = (mode == 3) ? 0.9999f : 1.0f;
			const bool  do_p1  = (mode == 1 || mode == 3);
			const bool  do_p2  = (mode == 1 || mode == 2 || mode == 3);

			// Step 0: full restore. If we snapshotted the main depth-stencil
			// at begin_segment (before any gun draw), copy it back now —
			// this wipes any gun depth that leaked into the main DSV via
			// engine paths our redirect didn't catch (the cause of the
			// MXAO right-side gun ghost on the floor). If snapshot path is
			// unsupported on this driver, this is a no-op and pass 1 / 2
			// are the only mitigation.
			if (g_have_depth_snapshot && g_depth_backup)
			{
				IDirect3DSurface9* main_depth = nullptr;
				if (SUCCEEDED(dev->GetDepthStencilSurface(&main_depth)) && main_depth)
				{
					HRESULT hr = dev->StretchRect(g_depth_backup, nullptr,
						main_depth, nullptr, D3DTEXF_NONE);
					if (FAILED(hr))
					{
						static bool s_logged = false;
						if (!s_logged)
						{
							s_logged = true;
							log::line("[depth_backup] StretchRect backup->main "
								"failed (hr=0x%08X); restore disabled.",
								(unsigned)hr);
						}
						g_depth_backup_unsupported = true;
					}
					main_depth->Release();
				}
				g_have_depth_snapshot = false;
			}

			IDirect3DStateBlock9* sb = nullptr;
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;

			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			dev->SetRenderState(D3DRS_ZENABLE,           TRUE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE,      TRUE);
			dev->SetRenderState(D3DRS_ZFUNC,             D3DCMP_ALWAYS);
			dev->SetRenderState(D3DRS_CULLMODE,          D3DCULL_NONE);
			dev->SetRenderState(D3DRS_LIGHTING,          FALSE);
			dev->SetRenderState(D3DRS_FOGENABLE,         FALSE);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE,  FALSE);
			dev->SetRenderState(D3DRS_ALPHATESTENABLE,   TRUE);
			dev->SetRenderState(D3DRS_ALPHAFUNC,         D3DCMP_GREATER);
			dev->SetRenderState(D3DRS_ALPHAREF,          0x10);
			dev->SetRenderState(D3DRS_STENCILENABLE,     FALSE);
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			dev->SetRenderState(D3DRS_SRGBWRITEENABLE,   FALSE);
			// disable color writes — we ONLY want depth to be written
			dev->SetRenderState(D3DRS_COLORWRITEENABLE,  0);

			dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetSamplerState(0, D3DSAMP_MINFILTER,   D3DTEXF_POINT);
			dev->SetSamplerState(0, D3DSAMP_MAGFILTER,   D3DTEXF_POINT);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSU,    D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSV,    D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			dev->SetTexture(0, g_tex);
			dev->SetVertexDeclaration(nullptr);
			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

			const float W = (float)g_w;
			const float H = (float)g_h;
			struct V { float x, y, z, rhw, u, v; };

			// Pass 1: clear original-gun ghost on the right side. UV not
			// flipped, z=far — every pixel the engine drew gun-alpha into
			// gets its main depth pushed to far so AO ignores it.
			if (do_p1)
			{
				V pass1[4] = {
					{ -0.5f,    -0.5f,    far_z, 1.0f, 0.0f, 0.0f },
					{  W-0.5f,  -0.5f,    far_z, 1.0f, 1.0f, 0.0f },
					{ -0.5f,     H-0.5f,  far_z, 1.0f, 0.0f, 1.0f },
					{  W-0.5f,   H-0.5f,  far_z, 1.0f, 1.0f, 1.0f },
				};
				dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, pass1, sizeof(V));
			}

			// Pass 2: stamp near depth on the mirrored side. UV flipped (u
			// inverted), z=0.0 — main depth at the visible mirror gun gets
			// near-plane depth so MXAO treats it as a foreground object.
			if (do_p2)
			{
				V pass2[4] = {
					{ -0.5f,    -0.5f,    0.0f, 1.0f, 1.0f, 0.0f },
					{  W-0.5f,  -0.5f,    0.0f, 1.0f, 0.0f, 0.0f },
					{ -0.5f,     H-0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
					{  W-0.5f,   H-0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
				};
				dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, pass2, sizeof(V));
			}

			if (sb) { sb->Apply(); sb->Release(); }
		}

		// Composite mirrored gun into the engine's currently-bound texture
		// (the tonemap source). Picks up the engine's grade automatically.
		bool inject_into_tonemap_source(IDirect3DDevice9* dev)
		{
			if (g_in_segment) end_segment(dev);
			if (!g_pass_active) return false;

			IDirect3DBaseTexture9* source_base = nullptr;
			IDirect3DTexture9*     source_tex  = nullptr;
			IDirect3DSurface9*     source_surf = nullptr;
			IDirect3DSurface9*     prev_color  = nullptr;
			IDirect3DSurface9*     prev_depth  = nullptr;
			IDirect3DStateBlock9*  sb          = nullptr;
			bool ok = false;
			const int blend_mode = engine::read_dvar_int(d_rtt_blend);
			const float W = (float)g_w;
			const float H = (float)g_h;
			struct V { float x, y, z, rhw, u, v; };
			V quad[4];

			if (FAILED(dev->GetTexture(0, &source_base)) || !source_base) goto cleanup;
			if (source_base->GetType() != D3DRTYPE_TEXTURE) goto cleanup;
			source_tex = static_cast<IDirect3DTexture9*>(source_base);
			source_tex->AddRef();
			if (FAILED(source_tex->GetSurfaceLevel(0, &source_surf)) || !source_surf) goto cleanup;
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;
			if (FAILED(dev->GetRenderTarget(0, &prev_color)) || !prev_color) goto cleanup;
			if (FAILED(dev->GetDepthStencilSurface(&prev_depth))) prev_depth = nullptr;

			dev->SetRenderTarget(0, source_surf);
			dev->SetDepthStencilSurface(nullptr);
			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			dev->SetRenderState(D3DRS_ZENABLE,           FALSE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE,      FALSE);
			dev->SetRenderState(D3DRS_CULLMODE,          D3DCULL_NONE);
			dev->SetRenderState(D3DRS_LIGHTING,          FALSE);
			dev->SetRenderState(D3DRS_FOGENABLE,         FALSE);
			dev->SetRenderState(D3DRS_SRGBWRITEENABLE,   FALSE);
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			dev->SetRenderState(D3DRS_COLORWRITEENABLE,
				D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
				D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
			dev->SetRenderState(D3DRS_STENCILENABLE,     FALSE);
			dev->SetRenderState(D3DRS_BLENDOP,           D3DBLENDOP_ADD);
			switch (blend_mode)
			{
			case 1:
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  TRUE);
				dev->SetRenderState(D3DRS_ALPHAREF,         1);
				dev->SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_GREATEREQUAL);
				break;
			case 2:
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
				break;
			case 3:
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  TRUE);
				dev->SetRenderState(D3DRS_ALPHAREF,         1);
				dev->SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_GREATEREQUAL);
				break;
			case 0:
			default:
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
				break;
			}
			dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetSamplerState(0, D3DSAMP_MINFILTER,  D3DTEXF_LINEAR);
			dev->SetSamplerState(0, D3DSAMP_MAGFILTER,  D3DTEXF_LINEAR);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSU,   D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSV,   D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			dev->SetTexture(0, g_tex);
			dev->SetVertexDeclaration(nullptr);
			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

			// UVs are horizontally inverted: u=1.0 on the left edge of the
			// destination, u=0.0 on the right -> the off-screen contents are
			// mirrored along the vertical axis when blitted.
			quad[0] = { -0.5f,    -0.5f,    0.0f, 1.0f, 1.0f, 0.0f };
			quad[1] = {  W-0.5f,  -0.5f,    0.0f, 1.0f, 0.0f, 0.0f };
			quad[2] = { -0.5f,     H-0.5f,  0.0f, 1.0f, 1.0f, 1.0f };
			quad[3] = {  W-0.5f,   H-0.5f,  0.0f, 1.0f, 0.0f, 1.0f };
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
			ok = true;

		cleanup:
			if (prev_color) { dev->SetRenderTarget(0, prev_color); prev_color->Release(); }
			if (prev_depth) { dev->SetDepthStencilSurface(prev_depth); prev_depth->Release(); }
			else            { dev->SetDepthStencilSurface(nullptr); }
			if (sb) { sb->Apply(); sb->Release(); }
			if (source_surf) source_surf->Release();
			if (source_tex)  source_tex->Release();
			if (source_base) source_base->Release();
			if (ok) g_pass_active = false;
			return ok;
		}

		// Composite mirrored off-screen onto the back-buffer (legacy path
		// when tonemap-inject is disabled or unavailable).
		void final_composite(IDirect3DDevice9* dev)
		{
			if (g_in_segment) end_segment(dev);
			if (!g_pass_active) return;
			g_pass_active = false;

			IDirect3DStateBlock9* sb = nullptr;
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;

			IDirect3DSurface9* prev_color = nullptr;
			IDirect3DSurface9* bb_surface = nullptr;
			bool bb_bound = false;
			if (SUCCEEDED(dev->GetRenderTarget(0, &prev_color)))
			{
				if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb_surface)))
				{
					if (prev_color != bb_surface)
					{
						dev->SetRenderTarget(0, bb_surface);
						bb_bound = true;
					}
				}
			}

			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			dev->SetRenderState(D3DRS_ZENABLE,          FALSE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
			dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
			dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
			dev->SetRenderState(D3DRS_FOGENABLE,        FALSE);

			const int srgb_mode  = engine::read_dvar_int(d_composite_srgb);
			const BOOL srgb_read  = (srgb_mode == 2 || srgb_mode == 3) ? TRUE : FALSE;
			const BOOL srgb_write = (srgb_mode == 1 || srgb_mode == 3) ? TRUE : FALSE;
			dev->SetRenderState(D3DRS_SRGBWRITEENABLE,  srgb_write);
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
			dev->SetRenderState(D3DRS_COLORWRITEENABLE,
				D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
				D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
			dev->SetRenderState(D3DRS_STENCILENABLE,    FALSE);

			const int blend_mode = engine::read_dvar_int(d_rtt_blend);
			dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
			switch (blend_mode)
			{
			case 1:
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  TRUE);
				dev->SetRenderState(D3DRS_ALPHAREF,         1);
				dev->SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_GREATEREQUAL);
				break;
			case 2:
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
				break;
			case 3:
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  TRUE);
				dev->SetRenderState(D3DRS_ALPHAREF,         1);
				dev->SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_GREATEREQUAL);
				break;
			case 0:
			default:
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
				break;
			}
			dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, srgb_read);
			dev->SetTexture(0, g_tex);
			dev->SetVertexDeclaration(nullptr);
			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

			const float W = (float)g_w;
			const float H = (float)g_h;
			struct V { float x, y, z, rhw, u, v; };
			V quad[4] = {
				{ -0.5f,    -0.5f,    0.0f, 1.0f, 1.0f, 0.0f },
				{  W-0.5f,  -0.5f,    0.0f, 1.0f, 0.0f, 0.0f },
				{ -0.5f,     H-0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
				{  W-0.5f,   H-0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
			};
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));

			if (bb_bound && prev_color) { dev->SetRenderTarget(0, prev_color); }
			if (bb_surface) bb_surface->Release();
			if (prev_color) prev_color->Release();
			if (sb) { sb->Apply(); sb->Release(); }
		}

		// ---- fullscreen mirror flip --------------------------------------
		void release_flip_target()
		{
			if (g_flip_surf) { g_flip_surf->Release(); g_flip_surf = nullptr; }
			if (g_flip_tex)  { g_flip_tex->Release();  g_flip_tex  = nullptr; }
			g_flip_w = g_flip_h = 0;
		}

		bool ensure_flip_target(IDirect3DDevice9* dev)
		{
			IDirect3DSurface9* bb = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
			D3DSURFACE_DESC bd; bb->GetDesc(&bd); bb->Release();
			if (g_flip_tex && (int)bd.Width == g_flip_w && (int)bd.Height == g_flip_h) return true;
			release_flip_target();
			if (FAILED(dev->CreateTexture(bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET,
				bd.Format, D3DPOOL_DEFAULT, &g_flip_tex, nullptr))) return false;
			if (FAILED(g_flip_tex->GetSurfaceLevel(0, &g_flip_surf))) { release_flip_target(); return false; }
			g_flip_w = (int)bd.Width;
			g_flip_h = (int)bd.Height;
			return true;
		}

		bool do_fullscreen_flip(IDirect3DDevice9* dev)
		{
			if (!ensure_flip_target(dev)) return false;
			IDirect3DSurface9* bb = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
			IDirect3DSurface9*    prev_color = nullptr;
			IDirect3DSurface9*    prev_depth = nullptr;
			IDirect3DStateBlock9* sb         = nullptr;
			bool ok = false;
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;
			if (FAILED(dev->GetRenderTarget(0, &prev_color))) prev_color = nullptr;
			if (FAILED(dev->GetDepthStencilSurface(&prev_depth))) prev_depth = nullptr;

			if (FAILED(dev->StretchRect(bb, nullptr, g_flip_surf, nullptr, D3DTEXF_NONE))) goto cleanup;

			dev->SetRenderTarget(0, bb);
			dev->SetDepthStencilSurface(nullptr);
			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			dev->SetRenderState(D3DRS_ZENABLE,           FALSE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE,      FALSE);
			dev->SetRenderState(D3DRS_CULLMODE,          D3DCULL_NONE);
			dev->SetRenderState(D3DRS_LIGHTING,          FALSE);
			dev->SetRenderState(D3DRS_FOGENABLE,         FALSE);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE,  FALSE);
			dev->SetRenderState(D3DRS_ALPHATESTENABLE,   FALSE);
			dev->SetRenderState(D3DRS_STENCILENABLE,     FALSE);
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
			dev->SetRenderState(D3DRS_SRGBWRITEENABLE,   FALSE);
			dev->SetRenderState(D3DRS_COLORWRITEENABLE,
				D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
				D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
			dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
			dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
			dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			dev->SetTexture(0, g_flip_tex);
			dev->SetVertexDeclaration(nullptr);
			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
			{
				const float W = (float)g_flip_w;
				const float H = (float)g_flip_h;
				struct V { float x, y, z, rhw, u, v; };
				V quad[4] = {
					{ -0.5f,    -0.5f,   0.0f, 1.0f, 1.0f, 0.0f },
					{  W-0.5f,  -0.5f,   0.0f, 1.0f, 0.0f, 0.0f },
					{ -0.5f,     H-0.5f, 0.0f, 1.0f, 1.0f, 1.0f },
					{  W-0.5f,   H-0.5f, 0.0f, 1.0f, 0.0f, 1.0f },
				};
				dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
			}
			ok = true;

		cleanup:
			if (prev_color) { dev->SetRenderTarget(0, prev_color); prev_color->Release(); }
			if (prev_depth) { dev->SetDepthStencilSurface(prev_depth); prev_depth->Release(); }
			else            { dev->SetDepthStencilSurface(nullptr); }
			if (sb) { sb->Apply(); sb->Release(); }
			if (bb) bb->Release();
			return ok;
		}

	} // anonymous namespace

	// ---- public entry points ---------------------------------------------
	void on_present(IDirect3DDevice9* /*dev*/)
	{
		lazy_init("Present");
		if (!g_logged_first_present)
		{
			g_logged_first_present = true;
			log::line("[present] hooks live (Present hook fired)");
		}

		// Frame-boundary safety: any RTT state should already be torn down
		// at EndScene; if not, drop it so the next frame starts clean.
		g_pass_active             = false;
		g_in_segment              = false;
		g_pending_early_composite = false;
		g_pending_fullmirror_flip = false;
	}

	void on_set_vertex_shader_constant_f(IDirect3DDevice9* dev, UINT start,
		const float* data, UINT count)
	{
		lazy_init("VSCF");
		if (!g_logged_first_vscf)
		{
			g_logged_first_vscf = true;
			log::line("[vscf] hook live (start=%u count=%u)", start, count);
		}
		g_vscf_calls++;
		if (!data || start != 0 || count != 4) return;
		const float c23 = data[11]; // c2[3]
		if (c23 < g_min_c23) g_min_c23 = c23;
		if (c23 > g_max_c23) g_max_c23 = c23;

		const int rtt_on = engine::read_dvar_int(d_rtt);
		if (!rtt_on) return;

		const bool is_dhp = (c23 < -0.02f && c23 > -0.50f);
		if (is_dhp)
		{
			g_vscf_dhp_hits++;
			if (!g_pass_active && !g_in_segment)
			{
				static unsigned s = 0;
				if (s++ < 8) log::line("[vscf] DHP gun pass START c23=%.6f", c23);
			}
			begin_segment(dev);
		}
		else if (g_in_segment)
		{
			// any non-dhp 4-row matrix while a gun segment is bound = transition
			// out of viewmodel pass (world / 2D HUD setup). Restore engine RT.
			static unsigned s = 0;
			if (s++ < 8) log::line("[vscf] gun pass END (c23=%.6f exits dhp)", c23);
			end_segment(dev);
		}
	}

	void on_set_pixel_shader_constant_f(IDirect3DDevice9* dev, UINT start,
		const float* data, UINT count)
	{
		lazy_init("PSCF");
		if (!g_logged_first_pscf)
		{
			g_logged_first_pscf = true;
			log::line("[pscf] hook live (start=%u count=%u)", start, count);
		}
		g_pscf_calls++;
		if (!data || start != 7 || count < 1) return;

		// PSCF c7 tonemap fingerprint: c70=c71=c72 in [-0.2, 0), c73 in (1, 5).
		const float c70 = data[0], c71 = data[1], c72 = data[2], c73 = data[3];
		auto eq = [](float a, float b){ float d = a - b; if (d < 0) d = -d; return d < 1e-4f; };
		const bool is_pre_hud = (c70 < 0.0f && c70 > -0.2f && eq(c70, c71) && eq(c70, c72)
			&& c73 > 1.0f && c73 < 5.0f);
		if (!is_pre_hud) return;

		g_pscf_tonemap_hits++;
		const int rtt_on    = engine::read_dvar_int(d_rtt);
		const int early     = engine::read_dvar_int(d_early_composite);
		const int tonemap   = engine::read_dvar_int(d_tonemap_inject);
		const int full_mirr = engine::read_dvar_int(d_full_mirror);

		static unsigned s_logged_match = 0;
		if (s_logged_match < 3)
		{
			s_logged_match++;
			log::line("[pscf] tonemap fingerprint MATCH c7=[%.4f,%.4f,%.4f,%.4f] "
				"rtt=%d early=%d inject=%d full=%d pass_active=%d",
				c70, c71, c72, c73, rtt_on, early, tonemap, full_mirr,
				(int)g_pass_active);
		}

		if (rtt_on && early != 0 && (g_pass_active || g_in_segment))
		{
			// Stamp main-depth corrections BEFORE we swap the render target
			// in inject_into_tonemap_source — at this point the engine's
			// main color + main depth-stencil are still bound, which is
			// exactly what apply_main_depth_fix needs (it only writes
			// depth, COLORWRITEENABLE=0). Skip cheaply if user disabled it.
			const int dfix_mode = engine::read_dvar_int(d_depth_fix);
			if (dfix_mode > 0)
			{
				if (g_in_segment) end_segment(dev);
				static unsigned s_dfx = 0;
				if (s_dfx++ < 4) log::line("[pscf] -> apply_main_depth_fix mode=%d", dfix_mode);
				apply_main_depth_fix(dev, dfix_mode);
			}

			if (tonemap)
			{
				{ static unsigned s = 0; if (s++ < 4) log::line("[pscf] -> inject_into_tonemap_source"); }
				if (!inject_into_tonemap_source(dev))
				{
					log::line("[pscf] inject FAILED, falling back to early composite");
					if (g_in_segment) end_segment(dev);
					g_pending_early_composite = true;
				}
			}
			else
			{
				if (g_in_segment) end_segment(dev);
				g_pending_early_composite = true;
			}
		}

		if (full_mirr == 1) g_pending_fullmirror_flip = true;
	}

	void on_after_draw(IDirect3DDevice9* dev)
	{
		if (g_pending_early_composite)
		{
			g_pending_early_composite = false;
			if (g_pass_active || g_in_segment)
			{
				{ static unsigned s = 0; if (s++ < 4) log::line("[after_draw] pending early composite -> final_composite"); }
				if (g_in_segment) end_segment(dev);
				final_composite(dev);
			}
		}
		if (g_pending_fullmirror_flip)
		{
			g_pending_fullmirror_flip = false;
			{ static unsigned s = 0; if (s++ < 4) log::line("[after_draw] pending fullmirror flip -> do_fullscreen_flip"); }
			do_fullscreen_flip(dev);
		}
	}

	void on_device_reset()
	{
		if (g_saved_color) { g_saved_color->Release(); g_saved_color = nullptr; }
		if (g_saved_depth) { g_saved_depth->Release(); g_saved_depth = nullptr; }
		g_pass_active             = false;
		g_in_segment              = false;
		g_pending_early_composite = false;
		g_pending_fullmirror_flip = false;
		release_targets();
		release_flip_target();
	}

	void on_end_scene(IDirect3DDevice9* dev)
	{
		lazy_init("EndScene");
		if (!g_logged_first_endscene)
		{
			g_logged_first_endscene = true;
			log::line("[end_scene] hook live (EndScene hook fired)");
		}

		// EndScene is our reliable per-frame hook (Present can be routed
		// around the wrapper on some IW3 paths). Heartbeat + frame-boundary
		// state reset live here.
		g_frame++;
		if ((g_frame % 60) == 0)
		{
			const int v_rtt    = engine::read_dvar_int(d_rtt);
			const int v_full   = engine::read_dvar_int(d_full_mirror);
			const int v_inject = engine::read_dvar_int(d_tonemap_inject);
			const int v_early  = engine::read_dvar_int(d_early_composite);
			log::line("[hb] frame=%u vscf=%u dhp=%u pscf=%u tonemap=%u "
				"rtt=%d full=%d inject=%d early=%d c23=[%.4f..%.4f]",
				g_frame, g_vscf_calls, g_vscf_dhp_hits, g_pscf_calls,
				g_pscf_tonemap_hits, v_rtt, v_full, v_inject, v_early,
				g_min_c23, g_max_c23);
			g_vscf_calls = g_pscf_calls = g_vscf_dhp_hits = g_pscf_tonemap_hits = 0;
			g_min_c23 = 1e9f; g_max_c23 = -1e9f;
		}

		// Safety-net composite: if neither tonemap-inject nor early-composite
		// fired this frame, blit the off-screen onto the BB now so the gun
		// is at least visible (even if it covers the HUD).
		if (g_pass_active || g_in_segment)
		{
			static unsigned s = 0;
			if (s++ < 4)
				log::line("[end_scene] safety-net final_composite (pass_active=%d in_seg=%d)",
					(int)g_pass_active, (int)g_in_segment);
			final_composite(dev);
		}
		// r_fullMirror == 2: brute-force flip everything (incl. HUD).
		const int full_mirr = engine::read_dvar_int(d_full_mirror);
		if (full_mirr == 2)
		{
			static unsigned s_log_throttle = 0;
			if ((s_log_throttle++ % 120) == 0)
				log::line("[end_scene] full_mirror=2 -> do_fullscreen_flip");
			do_fullscreen_flip(dev);
		}

		// Frame-boundary state reset (mirrors what Present used to do).
		g_pass_active             = false;
		g_in_segment              = false;
		g_pending_early_composite = false;
		g_pending_fullmirror_flip = false;
		// If a snapshot was taken this frame but no PSCF tonemap match
		// fired (e.g. tonemap path disabled, or the gun pass produced no
		// pixels), drop it so the next frame's begin_segment does a fresh
		// snapshot of an up-to-date world depth.
		g_have_depth_snapshot     = false;
	}
}

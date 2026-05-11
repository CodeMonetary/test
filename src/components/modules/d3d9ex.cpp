// https://github.com/IW4x/iw4x-client/blob/develop/src/Components/Modules/D3D9Ex.cpp
#include "std_include.hpp"

namespace components
{
	// r_mirrorViewmodel dump counters (defined in _renderer.cpp)
	extern void mirror_dump_inc_rs();
	extern void mirror_dump_inc_vscf();
	extern void mirror_dump_inc_pscf();
	extern void mirror_dump_inc_draw();

	// v35.8.1 diagnostic: monotonic frame counter for HUD-mirror logging.
	// Independent of /mirror_dump file capture; events are written to console.log
	// whenever r_mirrorViewmodel_log >= 2, including across grenade-damage frames
	// where the user cannot type /mirror_dump in time.
	static unsigned int s_hudlog_frame = 0;

	static inline int hudlog_level()
	{
		return dvars::r_mirrorViewmodel_log
			? dvars::r_mirrorViewmodel_log->current.integer : 0;
	}

	// ----------------------------------------------------------------------
	// r_mirrorViewmodel: render-to-texture mirror.
	//
	// On the depth-hack proj signature on c0-c3 (start of a viewmodel pass
	// segment), redirect color+depth to an off-screen texture matching
	// back-buffer size. The viewmodel renders unflipped (matrix-flip is
	// disabled in rtt mode), so tangents/normals/cull stay correct
	// internally. On the std-proj signature, switch back to the back-buffer
	// so the engine's world/HUD draws hit the visible target.
	//
	// v15: IW3 splits the viewmodel into TWO dhp/stdp segments per frame
	// (z-prefill before world, lit pass after world). Compositing at the
	// first stdp wrote a half-rendered gun (z-prefill only, mostly black)
	// onto the back-buffer, which is what produced the "ghost" appearance
	// in v12-v14. We now accumulate ALL segments into the off-screen target
	// (without clearing between segments so depth from z-prefill is reused
	// by the lit pass), and composite once at EndScene.
	// ----------------------------------------------------------------------
	namespace mirror_rtt
	{
		static IDirect3DTexture9* g_tex          = nullptr;
		static IDirect3DSurface9* g_color        = nullptr;
		static IDirect3DSurface9* g_depth        = nullptr;
		static IDirect3DSurface9* g_saved_color  = nullptr;
		static IDirect3DSurface9* g_saved_depth  = nullptr;
		static int  g_w                          = 0;
		static int  g_h                          = 0;
		static bool g_pass_active                = false; // first dhp seen this frame; cleared after final composite
		static bool g_in_segment                 = false; // off-screen RT currently bound
		static bool g_pending_early_composite    = false; // v22: set on PSCF c7 fingerprint, fires AFTER the next draw (the engine's final tonemap-output) instead of before it
		// v35.5: latched per-frame the moment final_composite() actually
		// runs (i.e. the gun-RTT pass is fully resolved onto the BB).
		// Reset in BeginScene. Used as a GATE for mirror_hud::begin_capture:
		// during damage-flash frames the engine emits a PSCF c7 fingerprint
		// match BEFORE world+gun rendering, so the first ALPHATESTENABLE=TRUE
		// (e.g. a transparency pass during world rendering) prematurely fires
		// begin_capture without this gate, and the whole scene ends up
		// captured into HUD-RTT (composite then mirrors world+gun+HUD).
		static bool g_final_composite_done_this_frame = false;
		// v35.8: latched the first time a depth-hack projection (gun matrix)
		// is uploaded this frame. Reset in BeginScene. Detected at SVP
		// regardless of r_mirrorViewmodel_rtt - dhp matrix shape is the
		// engine's own viewmodel-projection signal and fires whether or not
		// the gun-RTT path is active. Used as a gate for mirror_hud's PSCF
		// c7 arming: in damage-flash frames the engine emits an early c7
		// match BEFORE the gun pass starts; gating arming on dhp_seen
		// rejects that early-c7 (no fire), while still arming on a real
		// post-FX c7 that fires AFTER the gun pass (HUD captures normally).
		static bool g_dhp_seen_this_frame           = false;
		// v35.12: snapshot of g_dhp_seen_this_frame from the PREVIOUS frame.
		// Latched on BeginScene before the per-frame reset. Used by mirror_hud
		// PSCF c7 arming gate to ACCEPT c7 in death-cam frames where there is
		// no gun pass at all (dhp_seen stays false the whole frame). v35.8
		// alone rejects those, leaving the HUD un-mirrored during the entire
		// ~3-second death animation. Discriminator:
		//   damage early c7 : prev_dhp=true,  this_dhp=false at c7 -> REJECT
		//   death cam   c7  : prev_dhp=false, this_dhp=false at c7 -> ACCEPT
		//   normal      c7  : this_dhp=true (gun rendered) -> ACCEPT (v35.8)
		static bool g_dhp_seen_prev_frame           = false;
		// v35.11: full per-frame counters for dhp / segment / inject events
		// (the v35.8/v35.10 booleans were too coarse to detect anomalies).
		static int  g_dhp_count_this_frame          = 0;
		static int  g_begin_seg_count_this_frame    = 0;
		static int  g_end_seg_count_this_frame      = 0;
		static int  g_inject_calls_this_frame       = 0;
		static int  g_inject_ok_count_this_frame    = 0;

		// v32: full-screen mirror (`r_fullMirror`).
		//   0 = off
		//   1 = flip BB AFTER tonemap, BEFORE HUD => world+gun mirror, HUD intact
		//   2 = flip BB at EndScene             => everything including HUD
		static IDirect3DTexture9* g_flip_tex     = nullptr;
		static IDirect3DSurface9* g_flip_surf    = nullptr;
		static int  g_flip_w                     = 0;
		static int  g_flip_h                     = 0;
		static bool g_pending_fullmirror_flip    = false; // set on PSCF c7 fingerprint when fullMirror==1; fires AFTER the next draw

		// v37.2: pixel-shader-pointer cache for the engine's post-FX tonemap pass.
		// Latched on the FIRST successful structural match; used as a fallback
		// when the dvar settings push c7 outside even the widened structural
		// bounds. Weak reference (no AddRef), cleared on Reset(). Stale-pointer
		// comparisons are safe (numeric compare only, never dereferenced).
		static IDirect3DPixelShader9* g_tonemap_ps_cache = nullptr;

		// v37.2: tonemap-pass fingerprint - structural part only.
		// Same RGB on c70..c72, c70 slightly negative, c73 a positive gamma
		// exponent. Earlier bounds (-0.2<c70<0, 1<c73<5) were derived from
		// default film tweak settings only; the engine drives c7 from
		// r_contrast (-> c70) and r_desaturation (-> c73), so any non-default
		// setting moved c7 outside those bounds and broke detection.
		// Widened bounds capture all dvar combos observed in PR #4 diagnostics
		// (default: c70=-0.066/c73=2.77; r_contrast=2: c70=-0.766; r_desaturation=0:
		// c73=13.24) with margin.
		static inline bool match_tonemap_structural(float c70, float c71, float c72, float c73)
		{
			auto fapprox_eq = [](float a, float b) { float d = a - b; if (d < 0) d = -d; return d < 1e-4f; };
			return c70 < 0.0f && c70 > -1.5f
				&& fapprox_eq(c70, c71) && fapprox_eq(c70, c72)
				&& c73 > 0.5f && c73 < 20.0f;
		}

		// v37.2: tonemap signal = structural match OR (cached PS + RGB equality).
		// The cache fallback exists because users can push r_contrast / r_desaturation
		// far enough to move c7 outside the widened structural bounds. v37.0 made
		// the cache fallback unconditional on c7 shape, which caused over-detection
		// on r_fullMirror==1 and r_hudMirror==1: the engine reuses the tonemap PS
		// for secondary post-FX passes (sun fade / colour grade) that upload
		// c7=(3.78,4.00,3.56,1.00) - different RGB per component, structurally not
		// a tonemap signal, but the cache matched on PS pointer alone and produced
		// translucent ghosts. v37.2 keeps the cache but ALSO requires c70==c71==c72
		// in the cache branch, so the secondary passes (which use distinct per-mesh
		// lighting constants with non-equal RGB) are correctly rejected.
		static inline bool match_tonemap_signal(IDirect3DDevice9* dev,
			float c70, float c71, float c72, float c73)
		{
			auto fapprox_eq = [](float a, float b) { float d = a - b; if (d < 0) d = -d; return d < 1e-4f; };
			const bool same_rgb = fapprox_eq(c70, c71) && fapprox_eq(c70, c72);
			const bool structural = match_tonemap_structural(c70, c71, c72, c73);
			if (!dev) return structural;
			IDirect3DPixelShader9* cur = nullptr;
			if (FAILED(dev->GetPixelShader(&cur))) return structural;
			bool result = structural;
			if (structural)
			{
				if (!g_tonemap_ps_cache && cur) g_tonemap_ps_cache = cur; // weak ref, latch on first hit
			}
			else if (same_rgb && cur && cur == g_tonemap_ps_cache)
			{
				// cache fallback - RGB equality REQUIRED so per-mesh lighting
				// constants (3.78,4.00,3.56,1.00) are rejected even though they
				// may share the cached PS pointer with the real tonemap pass.
				result = true;
			}
			if (cur) cur->Release();
			return result;
		}

		static void release_targets()
		{
			if (g_color) { g_color->Release(); g_color = nullptr; }
			if (g_depth) { g_depth->Release(); g_depth = nullptr; }
			if (g_tex)   { g_tex->Release();   g_tex   = nullptr; }
			g_w = g_h = 0;
		}

		static bool ensure_targets(IDirect3DDevice9* dev)
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
			g_w = (int)bd.Width;
			g_h = (int)bd.Height;
			return true;
		}

		// Bind off-screen color+depth so the next batch of viewmodel draws lands there.
		// On the FIRST segment of a frame, also clear the off-screen target. Subsequent
		// segments must NOT clear, so the lit pass z-tests against z-prefill depth and
		// composes on top of the z-prefill color in the same target.
		static void begin_segment(IDirect3DDevice9* dev)
		{
			if (g_in_segment) return;
			if (!ensure_targets(dev)) return;
			if (FAILED(dev->GetRenderTarget(0, &g_saved_color))) { g_saved_color = nullptr; return; }
			if (FAILED(dev->GetDepthStencilSurface(&g_saved_depth))) { g_saved_depth = nullptr; }
			dev->SetRenderTarget(0, g_color);
			dev->SetDepthStencilSurface(g_depth);
			if (!g_pass_active)
			{
				dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
					0x00000000, 1.0f, 0);
				g_pass_active = true;
			}
			g_in_segment = true;
			++g_begin_seg_count_this_frame; // v35.11
		}

		// Switch back to engine's color+depth so post-viewmodel world draws are visible.
		// Does NOT composite; the off-screen target is preserved for further segments
		// or for the EndScene final composite.
		static void end_segment(IDirect3DDevice9* dev)
		{
			if (!g_in_segment) return;
			g_in_segment = false;
			if (g_saved_color) { dev->SetRenderTarget(0, g_saved_color); g_saved_color->Release(); g_saved_color = nullptr; }
			if (g_saved_depth) { dev->SetDepthStencilSurface(g_saved_depth); g_saved_depth->Release(); g_saved_depth = nullptr; }
			else                 dev->SetDepthStencilSurface(nullptr);
			++g_end_seg_count_this_frame; // v35.11
		}

		// v33 (ported from cod4mirror): rewrite main depth-stencil at the
		// gun pixels so post-process AO (ReShade MXAO/SSAO) does NOT bleed
		// wall shadows through the mirrored gun. Called between end_segment
		// (engine main RT+DSV are still bound) and inject_into_tonemap_source.
		// We sample our off-screen RT color (g_tex), use its alpha as a mask,
		// ZWRITE only.
		//
		//   Pass 1 (right side, original gun shape):
		//     UV not flipped, z=far. Stamps the leaked engine z-prefill of
		//     the original right-side gun out to the far plane.
		//
		//   Pass 2 (left side, mirrored gun):
		//     UV horizontally flipped, z=0.0 (near). Tells AO that the
		//     visible mirrored gun is a near-occluder.
		//
		// In practice pass 1 alone caused MXAO to draw a silhouette outline
		// at the right-side z-discontinuity, so default mode is 2
		// (pass 2 only). r_mirrorViewmodel_depthFix dvar selects:
		//   0 = off
		//   1 = pass 1 + pass 2 with z=1.0 (legacy ghost-creator)
		//   2 = pass 2 only (default; no right-side touch)
		//   3 = pass 1 + pass 2 with z=0.9999 (in case driver clips at 1.0)
		static void apply_main_depth_fix(IDirect3DDevice9* dev, int mode)
		{
			if (mode <= 0) return;
			if (!g_pass_active || !g_tex || g_w <= 0 || g_h <= 0) return;
			const float far_z = (mode == 3) ? 0.9999f : 1.0f;
			const bool  do_p1 = (mode == 1 || mode == 3);
			const bool  do_p2 = (mode == 1 || mode == 2 || mode == 3);

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
			// disable color writes - we ONLY want depth to be written
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


		static bool inject_into_tonemap_source(IDirect3DDevice9* dev)
		{
			++g_inject_calls_this_frame; // v35.11 (count includes pre-checks)
			if (g_in_segment) end_segment(dev);
			if (!g_pass_active) return false;

			IDirect3DBaseTexture9* source_base = nullptr;
			IDirect3DTexture9* source_tex = nullptr;
			IDirect3DSurface9* source_surface = nullptr;
			IDirect3DSurface9* prev_color = nullptr;
			IDirect3DSurface9* prev_depth = nullptr;
			IDirect3DStateBlock9* sb = nullptr;
			bool ok = false;
			const int blend_mode = dvars::r_mirrorViewmodel_rttBlend
				? dvars::r_mirrorViewmodel_rttBlend->current.integer : 2;
			const float W = (float)g_w;
			const float H = (float)g_h;
			struct V { float x, y, z, rhw, u, v; };
			V quad[4];

			if (FAILED(dev->GetTexture(0, &source_base)) || !source_base) goto cleanup;
			if (source_base->GetType() != D3DRTYPE_TEXTURE) goto cleanup;
			source_tex = static_cast<IDirect3DTexture9*>(source_base);
			source_tex->AddRef();
			if (FAILED(source_tex->GetSurfaceLevel(0, &source_surface)) || !source_surface) goto cleanup;
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;
			if (FAILED(dev->GetRenderTarget(0, &prev_color)) || !prev_color) goto cleanup;
			if (FAILED(dev->GetDepthStencilSurface(&prev_depth))) prev_depth = nullptr;

			dev->SetRenderTarget(0, source_surface);
			dev->SetDepthStencilSurface(nullptr);
			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			dev->SetRenderState(D3DRS_ZENABLE,          FALSE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
			dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
			dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
			dev->SetRenderState(D3DRS_FOGENABLE,        FALSE);
			dev->SetRenderState(D3DRS_SRGBWRITEENABLE,  FALSE);
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
			dev->SetRenderState(D3DRS_COLORWRITEENABLE,
				D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
				D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
			dev->SetRenderState(D3DRS_STENCILENABLE,    FALSE);

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
			dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			dev->SetTexture(0, g_tex);
			dev->SetVertexDeclaration(nullptr);
			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);

			quad[0] = { -0.5f,    -0.5f,    0.0f, 1.0f, 1.0f, 0.0f };
			quad[1] = {  W-0.5f,  -0.5f,    0.0f, 1.0f, 0.0f, 0.0f };
			quad[2] = { -0.5f,     H-0.5f,  0.0f, 1.0f, 1.0f, 1.0f };
			quad[3] = {  W-0.5f,   H-0.5f,  0.0f, 1.0f, 0.0f, 1.0f };
			dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
			ok = true;

		cleanup:
			if (prev_color) { dev->SetRenderTarget(0, prev_color); prev_color->Release(); prev_color = nullptr; }
			if (prev_depth) { dev->SetDepthStencilSurface(prev_depth); prev_depth->Release(); prev_depth = nullptr; }
			else            { dev->SetDepthStencilSurface(nullptr); }
			if (sb) { sb->Apply(); sb->Release(); }
			if (source_surface) { source_surface->Release(); source_surface = nullptr; }
			if (source_tex) { source_tex->Release(); source_tex = nullptr; }
			if (source_base) { source_base->Release(); source_base = nullptr; }
			if (ok)
			{
				g_pass_active = false;
				// v35.6: latch "gun pass resolved this frame" in the inject
				// path too. inject_into_tonemap_source is the default fast
				// path for r_mirrorViewmodel_rttTonemapInject=1 (default);
				// when it succeeds it sets g_pass_active=false directly so
				// final_composite() returns early on the next call and the
				// v35.5 latch was never set, leaving the HUD-RTT gate closed
				// for the entire frame. Set it here so the gate opens at the
				// real post-FX c7 (when g_pass_active was true at entry to
				// inject) but stays closed for the early-c7 false trigger
				// during damage flash (when g_pass_active was false at entry
				// and inject returned ok=false at the !g_pass_active early-out).
				g_final_composite_done_this_frame = true;
				++g_inject_ok_count_this_frame; // v35.11
			}
			return ok;
		}

		static void final_composite(IDirect3DDevice9* dev)
		{
			if (g_in_segment) end_segment(dev); // safety net (no stdp seen before EndScene)
			if (!g_pass_active) return;
			g_pass_active   = false;
			// v35.5: latch that gun-RTT has been resolved onto the BB this frame.
			// mirror_hud::begin_capture is gated on this flag (see SetRenderState
			// ALPHATESTENABLE hook) so HUD-RTT capture cannot start until the gun
			// pass is fully done. Prevents early-c7 false triggers during damage
			// flash from putting world+gun into HUD-RTT.
			g_final_composite_done_this_frame = true;

			// v14: capture ALL device state in a state block. After the composite we
			// Apply() the block which restores every render state, texture stage,
			// sampler, stream source, index buffer, vertex/pixel shader, FVF, etc.
			// This is necessary because DrawPrimitiveUP sets stream source 0 to NULL
			// which was causing subsequent engine draws to silently fail (observed as
			// world textures disappearing after the gun composite in v13).
			IDirect3DStateBlock9* sb = nullptr;
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;

			// v20: D3D9 state blocks do NOT capture render targets. When the early
			// composite path runs from the post-FX/HUD-boundary PSCF hook the
			// engine often has an intermediate post-effect render target bound
			// (e.g. PINGPONG or POST_EFFECT) that is later consumed as the
			// tonemap source. Compositing to that RT lets the tonemap pass eat
			// the gun pixels and the gun ends up invisible. Force the back-buffer
			// (the actual screen target) for the composite, then restore whatever
			// the engine had bound so the rest of the frame keeps working.
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
			// v24: gamma encoding for the composite. The engine's tonemap pass
			// bakes an sRGB-like curve into the back-buffer; the gun bypasses
			// that pass and pasting linear gun pixels straight onto the encoded
			// bb yields a slightly warm/desaturated 'sandy' tint. Encoding the
			// composite write (SRGBWRITE=TRUE) brings the gun in line with the
			// world. See r_mirrorViewmodel_compositeSrgb for details.
			const int srgb_mode = dvars::r_mirrorViewmodel_compositeSrgb
				? dvars::r_mirrorViewmodel_compositeSrgb->current.integer : 1;
			const BOOL srgb_read  = (srgb_mode == 2 || srgb_mode == 3) ? TRUE : FALSE;
			const BOOL srgb_write = (srgb_mode == 1 || srgb_mode == 3) ? TRUE : FALSE;
			dev->SetRenderState(D3DRS_SRGBWRITEENABLE,  srgb_write);
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
			dev->SetRenderState(D3DRS_COLORWRITEENABLE,
				D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
				D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
			dev->SetRenderState(D3DRS_STENCILENABLE,    FALSE);

			// v13: composite blend mode is selectable.
			const int blend_mode = dvars::r_mirrorViewmodel_rttBlend
				? dvars::r_mirrorViewmodel_rttBlend->current.integer : 2;
			dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
			switch (blend_mode)
			{
			case 1: // SRCALPHA/INVSRCALPHA + ALPHATEST > 0
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  TRUE);
				dev->SetRenderState(D3DRS_ALPHAREF,         1);
				dev->SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_GREATEREQUAL);
				break;
			case 2: // additive ONE/ONE
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
				break;
			case 3: // additive ONE/ONE + ALPHATEST > 0
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
				dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,  TRUE);
				dev->SetRenderState(D3DRS_ALPHAREF,         1);
				dev->SetRenderState(D3DRS_ALPHAFUNC,        D3DCMP_GREATEREQUAL);
				break;
			case 0: // SRCALPHA/INVSRCALPHA (legacy v12)
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

			// Restore the engine's render target before Apply (state blocks don't
			// touch RTs), then everything else captured in the state block (incl.
			// stream source 0, index buffer, vertex decl, shaders, all render
			// states, samplers).
			if (bb_bound && prev_color) { dev->SetRenderTarget(0, prev_color); }
			if (bb_surface) { bb_surface->Release(); bb_surface = nullptr; }
			if (prev_color) { prev_color->Release(); prev_color = nullptr; }
			if (sb) { sb->Apply(); sb->Release(); }
		}

		static void release_flip_target()
		{
			if (g_flip_surf) { g_flip_surf->Release(); g_flip_surf = nullptr; }
			if (g_flip_tex)  { g_flip_tex->Release();  g_flip_tex  = nullptr; }
			g_flip_w = g_flip_h = 0;
		}

		static bool ensure_flip_target(IDirect3DDevice9* dev)
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

		// v32: copy current backbuffer to a temp texture, then redraw the
		// backbuffer with horizontally inverted UVs. The result is a full-
		// screen horizontal flip of whatever was last drawn on the BB.
		static bool do_fullscreen_flip(IDirect3DDevice9* dev)
		{
			if (!ensure_flip_target(dev)) return false;
			IDirect3DSurface9* bb = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
			IDirect3DSurface9* prev_color = nullptr;
			IDirect3DSurface9* prev_depth = nullptr;
			IDirect3DStateBlock9* sb = nullptr;
			bool ok = false;
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;
			if (FAILED(dev->GetRenderTarget(0, &prev_color))) prev_color = nullptr;
			if (FAILED(dev->GetDepthStencilSurface(&prev_depth))) prev_depth = nullptr;

			// Copy current BB to flip texture
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

		static void on_device_reset()
		{
			if (g_saved_color) { g_saved_color->Release(); g_saved_color = nullptr; }
			if (g_saved_depth) { g_saved_depth->Release(); g_saved_depth = nullptr; }
			g_pass_active            = false;
			g_in_segment             = false;
			g_pending_early_composite = false;
			g_pending_fullmirror_flip = false;
			g_tonemap_ps_cache       = nullptr; // v37.2: weak ref, drop on device reset
			release_targets();
			release_flip_target();
		}
	}

	// ----------------------------------------------------------------------
	// r_hudMirror v35: HUD-only horizontal mirror via separate render target.
	//
	// v34 attempted to flip the HUD's 2D-ortho projection at VSCF level. That
	// failed because the engine reuses ONE 2D-ortho matrix for stencil-mask
	// setup, post-FX (bloom/sun-fade/color-grade tonemap), AND HUD - they are
	// indistinguishable by matrix content, so flipping it broke post-FX too
	// (sandy world, broken tonemap).
	//
	// v35 strategy: piggyback on the existing PSCF c7 fingerprint (= post-FX
	// done, HUD next, set in SetPixelShaderConstantF). After the engine's
	// final tonemap-output draw, redirect rendering to an off-screen HUD RTT.
	// All subsequent draws (the HUD pass) land on HUD RTT. In EndScene
	// wrapper we composite HUD RTT to back-buffer with optional UV-X flip
	// when r_hudMirror==1. Designed to combine with ReShade's Flip.fx, which
	// flips the entire final frame: with r_hudMirror=1 the HUD is pre-mirrored
	// at engine level, then Flip.fx mirrors the whole frame, so HUD reads
	// upright while world+gun stay mirrored. World+gun+post-FX are unchanged
	// because we only redirect the HUD pass, not the world/post-FX passes.
	// ----------------------------------------------------------------------
	namespace mirror_hud
	{
		static IDirect3DTexture9* g_tex             = nullptr;
		static IDirect3DSurface9* g_color           = nullptr;
		static IDirect3DSurface9* g_saved_color     = nullptr;
		static IDirect3DSurface9* g_saved_depth     = nullptr;
		static int  g_w                              = 0;
		static int  g_h                              = 0;
		static bool g_active                         = false;  // HUD RTT currently bound
		// v35.2: armed by PSCF c7 fingerprint (= post-FX in progress);
		// fired by SetRenderState(ALPHATESTENABLE, TRUE) - the strong
		// HUD-start signal. v35 fired begin_capture after the next draw
		// post-PSCF c7, but several post-FX fullscreen quads (color
		// grade, glow/flare additive) happen between the tonemap-output
		// draw and the actual HUD pass, so HUD-RTT was capturing the
		// scene image too and composite() mirrored the entire frame.
		static bool g_capture_armed                  = false;  // PSCF c7 fingerprint detected, waiting for ALPHATESTENABLE=TRUE
		// v35.1 diagnostic counters (printed at end-of-frame in dump)
		static int  g_pscf_hits_this_frame           = 0;     // PSCF c7 fingerprint matches in current frame
		static int  g_begin_calls_this_frame         = 0;     // successful begin_capture invocations
		static int  g_rt_redirects_this_frame        = 0;     // SetRenderTarget(0,X) intercepted while g_active
		static int  g_composite_calls_this_frame     = 0;     // composite() invocations
		static int  g_alphatest_fires_this_frame     = 0;     // v35.2: ALPHATESTENABLE=TRUE that fired begin_capture
		// v35.8.1 diagnostic counters: per-frame breakdown of c7 fingerprint
		// matches by arming-gate outcome.
		//   match    : total PSCF c7 calls whose values pass the (-x,-x,-x,gamma) shape
		//   armed    : matches that ALSO had mirror_rtt::g_dhp_seen_this_frame=true
		//              (i.e. arming gate accepted, capture would fire on next ALPHATESTENABLE)
		//   rejected : matches with dhp_seen=false (early/pre-gun c7, gate rejected -- HUD
		//              will NOT capture this frame, HUD ends up un-mirrored)
		static int  g_c7_match_this_frame            = 0;
		static int  g_c7_armed_this_frame            = 0;
		static int  g_c7_rejected_this_frame         = 0;
		// v35.9 diagnostic+fix: count dhp uploads that arrive AFTER
		// mirror_hud HUD-RTT capture has started (g_active=true). In
		// damage frames the engine emits a second gun pass after
		// begin_capture; mirror_rtt re-binds its off-screen RT through
		// the underlying device (bypassing our SetRenderTarget hook),
		// then end_segment restores HUD-RTT. The mirrored gun then
		// gets composited into HUD-RTT at EndScene (mirror_rtt::final_
		// composite blits onto whatever RT is bound), and mirror_hud::
		// composite() flips it horizontally on the back-buffer -- the
		// gun appears double-flipped (un-mirrored) for ~2s. The fix in
		// EndScene restores the BB before final_composite to prevent
		// this; this counter is purely for damage-frame verification.
		static int  g_late_dhp_this_frame            = 0;
		// v35.10 diagnostic counters. Goal: find the mechanism that
		// makes the gun appear UN-mirrored for ~2s during damage flash
		// when r_hudMirror=1. v35.9 hypothesis (late dhp upload after
		// begin_capture) was REFUTED by user log: late_dhp=0, rt=0 in
		// all 654 frames. Plus user tested flipFollow=9999 -- bug
		// persists, so VSCF flip-window exhaustion is also not it.
		// These counters track other suspect events:
		static int  g_inj_fail_this_frame            = 0; // inject_into_tonemap_source returned false (fallback to pending_early_composite)
		static int  g_early_comp_this_frame          = 0; // pending_early_composite triggered final_composite from Draw[Indexed]Primitive
		static int  g_comp_during_hud_this_frame     = 0; // final_composite ran while HUD-RTT was bound (g_active=true)
		static int  g_draws_during_hud_this_frame    = 0; // Draw[Indexed]Primitive while g_active=true (post-capture rendering volume)
		static int  g_mtx_flipreg_during_hud_this_frame = 0; // VSCF 4-row matrix upload at flipReg while g_active=true (suspected gun re-render)
		// v35.15: shader-draw escape mechanism for HUD-RTT.
		// During the damage flash / death cam a fullscreen post-FX
		// draw can run between begin_capture and the first real HUD
		// draw. With HUD-RTT bound that quad would write INTO HUD-RTT;
		// composite() then UV-flips HUD-RTT onto BB so the sampled
		// scene appears mirrored (the world-flip bug). The escape
		// detects this draw via the stage-0 texture (a large render-
		// target sized >= 512) and redirects it to g_saved_color (BB)
		// for the duration of that single draw, then HUD-RTT is
		// re-bound.
		static bool g_last_tex0_is_big_rt                   = false;
		static int  g_shader_escapes_this_frame             = 0;
		static bool g_logged_first_shader_escape_this_frame = false;

		// KNOWN LIMITATION: at r_hudMirror=1 the killstreak / nickname
		// outline shader (custom VS+PS, sabe=TRUE, srca=INVDESTALPHA,
		// dsta=ZERO, stencil disabled) renders into HUD-RTT with a
		// visibly bolder dark outline than at r_hudMirror=0. Three
		// alpha-math fix attempts (v35.16 SEPARATEALPHABLENDENABLE
		// global, v35.19 SetRenderState alpha-factor override, v35.20
		// per-draw alpha-factor force) all failed: v35.20 did fire on
		// every outline draw (43-68/frame in user log) but produced
		// no visible change, and v35.16 / v35.20 also leaked state
		// into world / gun rendering and broke weapon visibility
		// during fire. v35.21 stencil hypothesis was also refuted by
		// log (STENCILENABLE=0 in all 983 outline draws). The visible
		// difference is probably caused by a stage>0 sampler or a
		// shader constant that depends on RT identity, but verifying
		// would require deeply invasive shader-resource inspection.
		// Current behaviour kept: full HUD mirror including outline
		// text, with cosmetic bolder outline as a known limitation.

		static void release_targets()
		{
			if (g_color) { g_color->Release(); g_color = nullptr; }
			if (g_tex)   { g_tex->Release();   g_tex   = nullptr; }
			g_w = g_h = 0;
		}

		static bool ensure_targets(IDirect3DDevice9* dev)
		{
			IDirect3DSurface9* bb = nullptr;
			if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
			D3DSURFACE_DESC bd; bb->GetDesc(&bd); bb->Release();
			if (g_tex && (int)bd.Width == g_w && (int)bd.Height == g_h) return true;
			release_targets();
			// A8R8G8B8 (alpha for HUD transparency). HUD does not z-test in iw3,
			// so no depth-stencil is needed.
			if (FAILED(dev->CreateTexture(bd.Width, bd.Height, 1, D3DUSAGE_RENDERTARGET,
				D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_tex, nullptr))) return false;
			if (FAILED(g_tex->GetSurfaceLevel(0, &g_color))) { release_targets(); return false; }
			g_w = (int)bd.Width;
			g_h = (int)bd.Height;
			return true;
		}

		// Begin capturing HUD draws. Save current RT/DSV, bind HUD RTT, clear
		// to (0,0,0,0) so the composite alpha-blend later writes only the
		// HUD-drawn pixels onto the back-buffer. Called once per frame, right
		// after the engine's final tonemap-output draw (see PSCF c7 hook).
		static void begin_capture(IDirect3DDevice9* dev)
		{
			if (g_active) return;
			if (!ensure_targets(dev)) return;
			if (FAILED(dev->GetRenderTarget(0, &g_saved_color))) { g_saved_color = nullptr; return; }
			if (FAILED(dev->GetDepthStencilSurface(&g_saved_depth))) { g_saved_depth = nullptr; }
			dev->SetRenderTarget(0, g_color);
			dev->SetDepthStencilSurface(nullptr); // HUD does not z-test
			dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
			g_active = true;
			++g_begin_calls_this_frame;
		}

		// Restore back-buffer as RT and composite HUD RTT onto it. mirror_x
		// controls horizontal flip: false = identity copy, true = UV-X mirror.
		// Uses standard premultiplied SRCALPHA / INVSRCALPHA blend so HUD
		// transparent areas keep the world+gun behind them.
		static void composite(IDirect3DDevice9* dev, bool mirror_x)
		{
			if (!g_active) return;
			g_active = false;
			++g_composite_calls_this_frame;
			IDirect3DStateBlock9* sb = nullptr;
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;

			// Restore engine RT/DSV (this is what the back-buffer or whatever
			// the engine had bound when post-FX completed). The composite below
			// will draw onto whatever RT is bound after restore; for the
			// non-Flip.fx case this is typically the back-buffer.
			if (g_saved_color) { dev->SetRenderTarget(0, g_saved_color); g_saved_color->Release(); g_saved_color = nullptr; }
			if (g_saved_depth) { dev->SetDepthStencilSurface(g_saved_depth); g_saved_depth->Release(); g_saved_depth = nullptr; }
			else                 dev->SetDepthStencilSurface(nullptr);

			dev->SetVertexShader(nullptr);
			dev->SetPixelShader(nullptr);
			dev->SetRenderState(D3DRS_ZENABLE,           FALSE);
			dev->SetRenderState(D3DRS_ZWRITEENABLE,      FALSE);
			dev->SetRenderState(D3DRS_CULLMODE,          D3DCULL_NONE);
			dev->SetRenderState(D3DRS_LIGHTING,          FALSE);
			dev->SetRenderState(D3DRS_FOGENABLE,         FALSE);
			dev->SetRenderState(D3DRS_ALPHABLENDENABLE,  TRUE);
			dev->SetRenderState(D3DRS_BLENDOP,           D3DBLENDOP_ADD);
			// v35.3: premultiplied-alpha composite. HUD elements drawing
			// into the cleared (0,0,0,0) HUD-RTT with their own SRCALPHA
			// blend already premultiply color by alpha (color = src.a *
			// src.color, alpha = src.a). Using SRCALPHA again here would
			// re-multiply by alpha and produce a darker / grayer HUD
			// (most visible on small low-alpha icons). ONE/INVSRCALPHA
			// is the standard composite for premultiplied RTTs.
			dev->SetRenderState(D3DRS_SRCBLEND,          D3DBLEND_ONE);
			dev->SetRenderState(D3DRS_DESTBLEND,         D3DBLEND_INVSRCALPHA);
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
			dev->SetTexture(0, g_tex);
			dev->SetVertexDeclaration(nullptr);
			dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
			{
				const float W = (float)g_w;
				const float H = (float)g_h;
				struct V { float x, y, z, rhw, u, v; };
				const float u0 = mirror_x ? 1.0f : 0.0f;
				const float u1 = mirror_x ? 0.0f : 1.0f;
				V quad[4] = {
					{ -0.5f,    -0.5f,   0.0f, 1.0f, u0, 0.0f },
					{  W-0.5f,  -0.5f,   0.0f, 1.0f, u1, 0.0f },
					{ -0.5f,     H-0.5f, 0.0f, 1.0f, u0, 1.0f },
					{  W-0.5f,   H-0.5f, 0.0f, 1.0f, u1, 1.0f },
				};
				dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
			}
			if (sb) { sb->Apply(); sb->Release(); }
		}

		static void on_device_reset()
		{
			if (g_saved_color) { g_saved_color->Release(); g_saved_color = nullptr; }
			if (g_saved_depth) { g_saved_depth->Release(); g_saved_depth = nullptr; }
			g_active                = false;
			g_capture_armed = false;
			release_targets();
		}
	}

#pragma region D3D9Device

	HRESULT d3d9ex::D3D9Device::QueryInterface(REFIID riid, void** ppvObj)
	{
		*ppvObj = nullptr;

		HRESULT hRes = m_pIDirect3DDevice9->QueryInterface(riid, ppvObj);
		if (hRes == NOERROR) *ppvObj = this;
		return hRes;
	}

	ULONG d3d9ex::D3D9Device::AddRef()
	{
		return m_pIDirect3DDevice9->AddRef();
	}

	ULONG d3d9ex::D3D9Device::Release()
	{
		game::glob::loaded_main_menu = false;
		game::glob::mainmenu_fade_done = false;

		ULONG count = m_pIDirect3DDevice9->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT d3d9ex::D3D9Device::TestCooperativeLevel()
	{
		return m_pIDirect3DDevice9->TestCooperativeLevel();
	}

	UINT d3d9ex::D3D9Device::GetAvailableTextureMem()
	{
		return m_pIDirect3DDevice9->GetAvailableTextureMem();
	}

	HRESULT d3d9ex::D3D9Device::EvictManagedResources()
	{
		return m_pIDirect3DDevice9->EvictManagedResources();
	}

	HRESULT d3d9ex::D3D9Device::GetDirect3D(IDirect3D9** ppD3D9)
	{
		return m_pIDirect3DDevice9->GetDirect3D(ppD3D9);
	}

	HRESULT d3d9ex::D3D9Device::GetDeviceCaps(D3DCAPS9* pCaps)
	{
		return m_pIDirect3DDevice9->GetDeviceCaps(pCaps);
	}

	HRESULT d3d9ex::D3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3DDevice9->GetDisplayMode(iSwapChain, pMode);
	}

	HRESULT d3d9ex::D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
	{
		return m_pIDirect3DDevice9->GetCreationParameters(pParameters);
	}

	HRESULT d3d9ex::D3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap)
	{
		return m_pIDirect3DDevice9->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
	}

	void d3d9ex::D3D9Device::SetCursorPosition(int X, int Y, DWORD Flags)
	{
		return m_pIDirect3DDevice9->SetCursorPosition(X, Y, Flags);
	}

	BOOL d3d9ex::D3D9Device::ShowCursor(BOOL bShow)
	{
		return m_pIDirect3DDevice9->ShowCursor(bShow);
	}

	HRESULT d3d9ex::D3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain)
	{
		return m_pIDirect3DDevice9->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
	}

	HRESULT d3d9ex::D3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain)
	{
		return m_pIDirect3DDevice9->GetSwapChain(iSwapChain, pSwapChain);
	}

	UINT d3d9ex::D3D9Device::GetNumberOfSwapChains()
	{
		return m_pIDirect3DDevice9->GetNumberOfSwapChains();
	}

	HRESULT d3d9ex::D3D9Device::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
	{
		// r_mirrorViewmodel v12: release POOL_DEFAULT off-screen targets before Reset.
		mirror_rtt::on_device_reset();

		if (components::active.gui)
		{
			if (GGUI_READY)
			{
				ImGui_ImplDX9_InvalidateDeviceObjects();
				auto hr = m_pIDirect3DDevice9->Reset(pPresentationParameters);
				ImGui_ImplDX9_CreateDeviceObjects();
			}
		}

		return m_pIDirect3DDevice9->Reset(pPresentationParameters);
	}

	HRESULT d3d9ex::D3D9Device::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion)
	{
		// r_mirrorViewmodel: clear the viewmodel flag at frame boundary so next
		// frame's world pass isn't rendered with inverted culling.
		// NOTE: EndScene owns the dump-frame counter. Some IW3 dispatch paths route
		// Present() around this wrapper, so relying on it alone drops frame boundaries.
		_renderer::mirror_viewmodel_active = false;
		// v35: clear the obsolete gun_seen flag (kept for ABI/fields but
		// no longer gates anything) and the HUD capture-start pending
		// flag at frame boundary as a belt-and-suspenders cleanup.
		_renderer::gun_seen_this_present = false;
		mirror_hud::g_capture_armed = false;
		return m_pIDirect3DDevice9->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
	}

	HRESULT d3d9ex::D3D9Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
	{
		return m_pIDirect3DDevice9->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);
	}

	HRESULT d3d9ex::D3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus)
	{
		return m_pIDirect3DDevice9->GetRasterStatus(iSwapChain, pRasterStatus);
	}

	HRESULT d3d9ex::D3D9Device::SetDialogBoxMode(BOOL bEnableDialogs)
	{
		return m_pIDirect3DDevice9->SetDialogBoxMode(bEnableDialogs);
	}

	void d3d9ex::D3D9Device::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp)
	{
		return m_pIDirect3DDevice9->SetGammaRamp(iSwapChain, Flags, pRamp);
	}

	void d3d9ex::D3D9Device::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp)
	{
		return m_pIDirect3DDevice9->GetGammaRamp(iSwapChain, pRamp);
	}

	HRESULT d3d9ex::D3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
	{
		if (Pool == D3DPOOL_MANAGED) { Pool = D3DPOOL_DEFAULT; Usage |= D3DUSAGE_DYNAMIC; }

		return m_pIDirect3DDevice9->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle)
	{
		if (Pool == D3DPOOL_MANAGED) { Pool = D3DPOOL_DEFAULT; Usage |= D3DUSAGE_DYNAMIC; }

		return m_pIDirect3DDevice9->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle)
	{
		if (Pool == D3DPOOL_MANAGED) { Pool = D3DPOOL_DEFAULT; Usage |= D3DUSAGE_DYNAMIC; }

		return m_pIDirect3DDevice9->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
	{
		if (Pool == D3DPOOL_MANAGED) { Pool = D3DPOOL_DEFAULT; Usage |= D3DUSAGE_DYNAMIC; }

		return m_pIDirect3DDevice9->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle)
	{
		if (Pool == D3DPOOL_MANAGED) { Pool = D3DPOOL_DEFAULT; Usage |= D3DUSAGE_DYNAMIC; }

		return m_pIDirect3DDevice9->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		return m_pIDirect3DDevice9->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		//return D3D_OK;
		return m_pIDirect3DDevice9->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint)
	{
		return m_pIDirect3DDevice9->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
	}

	HRESULT d3d9ex::D3D9Device::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture)
	{
		return m_pIDirect3DDevice9->UpdateTexture(pSourceTexture, pDestinationTexture);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
	{
		return m_pIDirect3DDevice9->GetRenderTargetData(pRenderTarget, pDestSurface);
	}

	HRESULT d3d9ex::D3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface)
	{
		return m_pIDirect3DDevice9->GetFrontBufferData(iSwapChain, pDestSurface);
	}

	HRESULT d3d9ex::D3D9Device::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
	{
		return m_pIDirect3DDevice9->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
	}

	HRESULT d3d9ex::D3D9Device::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color)
	{
		return m_pIDirect3DDevice9->ColorFill(pSurface, pRect, color);
	}

	HRESULT d3d9ex::D3D9Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		if (Pool == D3DPOOL_MANAGED) { Pool = D3DPOOL_DEFAULT; }

		return m_pIDirect3DDevice9->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
	}

	HRESULT d3d9ex::D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
	{
		// v35.1: while HUD-RTT capture is active, intercept index-0 RT
		// rebinds. The iw3 HUD pass starts with the engine binding the
		// back-buffer (or its current pingpong) again right after the
		// final tonemap-output draw. Without this hook, the engine's
		// rebind undoes the begin_capture()-time RT switch and HUD
		// draws land on the back-buffer instead of mirror_hud::g_color.
		// We update g_saved_color to whatever the engine wanted, so
		// composite() at EndScene restores the correct surface.
		if (RenderTargetIndex == 0 && mirror_hud::g_active && pRenderTarget && pRenderTarget != mirror_hud::g_color)
		{
			if (mirror_hud::g_saved_color) { mirror_hud::g_saved_color->Release(); mirror_hud::g_saved_color = nullptr; }
			mirror_hud::g_saved_color = pRenderTarget;
			mirror_hud::g_saved_color->AddRef();
			++mirror_hud::g_rt_redirects_this_frame;
			return m_pIDirect3DDevice9->SetRenderTarget(RenderTargetIndex, mirror_hud::g_color);
		}
		return m_pIDirect3DDevice9->SetRenderTarget(RenderTargetIndex, pRenderTarget);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget)
	{
		return m_pIDirect3DDevice9->GetRenderTarget(RenderTargetIndex, ppRenderTarget);
	}

	HRESULT d3d9ex::D3D9Device::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
	{
		return m_pIDirect3DDevice9->SetDepthStencilSurface(pNewZStencil);
	}

	HRESULT d3d9ex::D3D9Device::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface)
	{
		return m_pIDirect3DDevice9->GetDepthStencilSurface(ppZStencilSurface);
	}

	HRESULT d3d9ex::D3D9Device::BeginScene()
	{
		// r_mirrorViewmodel: belt-and-suspenders; ensure flag is clear at frame start
		// so the world pass (first after BeginScene) renders with normal culling.
		_renderer::mirror_viewmodel_active = false;

		// v35.5: clear the per-frame "gun pass resolved" latch. Used as
		// gate for HUD-RTT begin_capture so we never start capturing
		// HUD draws until mirror_rtt::final_composite has actually run
		// for this frame (or rtt is disabled, see ALPHATESTENABLE hook).
		mirror_rtt::g_final_composite_done_this_frame = false;
		// v35.12: latch previous-frame dhp_seen BEFORE we clear this-frame.
		// Read by mirror_hud c7 arming gate to relax v35.8 in death cam.
		mirror_rtt::g_dhp_seen_prev_frame = mirror_rtt::g_dhp_seen_this_frame;
		// v35.8: clear the per-frame "dhp seen" latch. Set at SVP when
		// the engine first uploads a depth-hack-projection (gun) matrix.
		mirror_rtt::g_dhp_seen_this_frame = false;
		mirror_rtt::g_dhp_count_this_frame       = 0; // v35.11
		mirror_rtt::g_begin_seg_count_this_frame = 0; // v35.11
		mirror_rtt::g_end_seg_count_this_frame   = 0; // v35.11
		mirror_rtt::g_inject_calls_this_frame    = 0; // v35.11
		mirror_rtt::g_inject_ok_count_this_frame = 0; // v35.11

		// v35.8.1 diagnostic: reset HUD-RTT per-frame counters here so values
		// represent ONLY the current frame even when /mirror_dump is OFF. The
		// previous code reset only inside the dump-active block in EndScene,
		// so the FIRST captured frame inherited accumulated counts from every
		// prior frame since process start (visible as bogus pscf_hits=2359 in
		// frame 0 of mirror_dump_20260428_015927.txt). Reset at frame START is
		// authoritative.
		mirror_hud::g_pscf_hits_this_frame       = 0;
		mirror_hud::g_alphatest_fires_this_frame = 0;
		mirror_hud::g_begin_calls_this_frame     = 0;
		mirror_hud::g_rt_redirects_this_frame    = 0;
		mirror_hud::g_composite_calls_this_frame = 0;

		// v35.8.1 diagnostic counters (cumulative within this frame, used for
		// the EndScene log line). Tracked separately from existing dump
		// counters so we can attribute c7 fingerprint matches that were
		// rejected by the v35.8 dhp_seen arming gate vs ones that armed.
		mirror_hud::g_c7_match_this_frame      = 0;
		mirror_hud::g_c7_armed_this_frame      = 0;
		mirror_hud::g_c7_rejected_this_frame   = 0;
		mirror_hud::g_late_dhp_this_frame      = 0;
		mirror_hud::g_inj_fail_this_frame             = 0;
		mirror_hud::g_early_comp_this_frame           = 0;
		mirror_hud::g_comp_during_hud_this_frame      = 0;
		mirror_hud::g_draws_during_hud_this_frame     = 0;
		mirror_hud::g_mtx_flipreg_during_hud_this_frame = 0;
		mirror_hud::g_shader_escapes_this_frame              = 0;     // v35.15
		mirror_hud::g_logged_first_shader_escape_this_frame  = false; // v35.15

		++s_hudlog_frame;

		if (_renderer::mirror_dump_active())
		{
			_renderer::mirror_dump_write("\n=== BEGIN frame %d (BeginScene) ===\n",
				_renderer::mirror_dump_frame_counter);
		}
		return m_pIDirect3DDevice9->BeginScene();
	}

	HRESULT d3d9ex::D3D9Device::EndScene()
	{
		// r_mirrorViewmodel v15: composite the accumulated viewmodel render once per
		// frame, regardless of how many dhp/stdp segments fired. Compositing at every
		// stdp transition (v12-v14) only blitted partial gun renders (z-prefill alone
		// for the first segment) and produced the "ghost" appearance.
		if (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment)
		{
			// v35.9 fix: in damage frames the engine emits a SECOND gun
			// pass AFTER mirror_hud::begin_capture has bound HUD-RTT. The
			// gun renders into mirror_rtt::g_color, end_segment restores
			// HUD-RTT (since begin_segment captured it as the saved RT),
			// and final_composite below would blit the mirrored gun into
			// HUD-RTT. mirror_hud::composite() then flips HUD-RTT to BB,
			// double-flipping the gun (gun appears UN-mirrored on screen
			// for ~2s during the damage flash). Restore BB binding here
			// so final_composite blits gun onto BB directly, then re-bind
			// HUD-RTT so mirror_hud::composite() at line below has the
			// expected state. Normal frames take the else branch since
			// inject_into_tonemap_source already set g_pass_active=false
			// during post-FX (this whole if-block is skipped), so v35.9
			// is a no-op outside the damage path.
			if (mirror_hud::g_active && mirror_hud::g_saved_color)
			{
				// v35.10 diagnostic: this is the v35.9 BB-restore path. Count
				// it whenever mirror_rtt has a pending gun pass at EndScene
				// while HUD-RTT is bound -- the bug-trigger condition.
				if (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment)
					++mirror_hud::g_comp_during_hud_this_frame;
				m_pIDirect3DDevice9->SetRenderTarget(0, mirror_hud::g_saved_color);
				mirror_rtt::final_composite(m_pIDirect3DDevice9);
				m_pIDirect3DDevice9->SetRenderTarget(0, mirror_hud::g_color);
			}
			else
			{
				mirror_rtt::final_composite(m_pIDirect3DDevice9);
			}
		}

		// v35: HUD-RTT composite. If r_hudMirror==1 the HUD pass was
		// redirected to mirror_hud::g_color after PSCF c7 fingerprint.
		// Restore the engine's RT/DSV (back-buffer) and composite the
		// HUD RTT onto it with horizontal UV flip. Composite must run
		// BEFORE gui::render_loop so ImGui (debug overlays) draws on top
		// of the HUD without being mirrored. Composite must run BEFORE
		// the r_fullMirror=2 EndScene flip so a user combining both
		// dvars sees HUD pre-mirrored then full-frame flipped (yielding
		// upright HUD over mirrored world+gun, same as Flip.fx flow).
		if (mirror_hud::g_active)
		{
			const int hud_mirror_eos = dvars::r_hudMirror
				? dvars::r_hudMirror->current.integer : 0;
			mirror_hud::composite(m_pIDirect3DDevice9, hud_mirror_eos == 1);
		}

		if (components::active.gui)
		{
			gui::render_loop();
		}

		// r_mirrorViewmodel: fallback frame boundary. Some builds route Present() around our
		// wrapper (observed in mirror_dump_20260424: 0 Present() hits vs 7 BeginScene). EndScene
		// is always called before Present and always reaches our wrapper, so it is a reliable
		// per-frame hook. Reset the viewmodel flag here too, and advance the dump counter.
		_renderer::mirror_viewmodel_active = false;
		// v35: same fallback reset for HUD pending flag (Present() may be
		// bypassed by some iw3 dispatch paths). gun_seen flag is obsolete
		// (v35 HUD-RTT path replaces v34 VSCF projection-flip path).
		_renderer::gun_seen_this_present = false;
		mirror_hud::g_capture_armed = false;

		// v35.8.1 diagnostic: per-frame HUD-mirror summary to console.log
		// (independent of /mirror_dump). Gated on r_mirrorViewmodel_log>=2 so
		// it can run during a real grenade-damage frame -- the user cannot type
		// /mirror_dump fast enough to catch the 2-second damage flash.
		//   c7      : total c7 fingerprint matches
		//   armed   : matches that passed dhp_seen gate (would arm capture)
		//   reject  : matches rejected by gate (HUD will not capture)
		//   fire    : ALPHATESTENABLE=TRUE that fired begin_capture
		//   beg     : successful begin_capture invocations
		//   comp    : composite() invocations
		//   dhp     : g_dhp_seen_this_frame at EndScene
		//   final   : g_final_composite_done_this_frame at EndScene
		//   hud_act : g_active at EndScene (HUD-RTT was bound)
		//   late_dhp: v35.9 -- dhp uploads after begin_capture (>=1 in damage
		//             frames; trigger for the v35.9 EndScene fix)
		//   rt      : v35.9 -- SetRenderTarget redirects to HUD-RTT this frame
		//   inj_fail: v35.10 -- inject_into_tonemap_source returned false
		//             (fallback to pending_early_composite path)
		//   early_comp: v35.10 -- pending_early_composite consumed by Draw
		//   comp_in_hud: v35.10 -- final_composite ran while HUD-RTT bound
		//             (this is the v35.9 fix's trigger condition; should
		//             be 0 in normal frames; non-zero in damage means the
		//             v35.9 BB-restore path was taken at EndScene)
		//   draws_in_hud: v35.10 -- Draw[Indexed]Primitive while HUD-RTT
		//             bound. Higher in damage frames would point at extra
		//             rendering happening into HUD-RTT (post-capture work).
		//   mtx64_in_hud: v35.10 -- VSCF 4-row matrix upload at flipReg
		//             while HUD-RTT bound. >=1 strongly suggests a stealth
		//             gun re-render after begin_capture: the gun matrix is
		//             flipped, gun renders into HUD-RTT, composite() flips
		//             horizontally, double-flip => un-mirrored gun on BB.
		//   dhp_n   : v35.11 -- total dhp_upload events this frame (>1 means
		//             multiple gun-pass starts, e.g. damage flinch re-render)
		//   bsg/esg : v35.11 -- begin_segment/end_segment call counts
		//   inj     : v35.11 -- inject_into_tonemap_source ok/total counts
		//   follow_end: v35.11 -- mirror_vscf_follow_remaining at SUMMARY
		//             (should be 0 if gun pass fully resolved; non-zero =>
		//             flip-window left armed past end of gun pass)
		//   pass_end: v35.11 -- mirror_rtt::g_pass_active at SUMMARY
		//             (should be 0 if final_composite ran; 1 => gun pass
		//             never composited, scheduled for EndScene fallback)
		if (hudlog_level() >= 2)
		{
			// v35.11: print a one-shot snapshot of mirror-related dvars the
			// first time hudlog is active. Verifies the test config matches
			// expectations (rtt, mirrorFx, hudMirror, flipReg, flipFollow).
			static bool s_dvar_snapshot_emitted = false;
			if (!s_dvar_snapshot_emitted)
			{
				s_dvar_snapshot_emitted = true;
				const auto iv = [](game::dvar_s* d){ return d ? d->current.integer : -1; };
				game::Com_PrintMessage(0, utils::va(
					"[hudlog] DVAR SNAPSHOT r_hudMirror=%d r_mirrorViewmodel_rtt=%d "
					"method=%d mirrorFx=%d flipReg=%d flipFollow=%d flipVSCF=%d "
					"depthFix=%d cullFix=%d rttBlend=%d rttTonemapInject=%d\n",
					iv(dvars::r_hudMirror),
					iv(dvars::r_mirrorViewmodel_rtt),
					iv(dvars::r_mirrorViewmodel_method),
					iv(dvars::r_mirrorViewmodel_mirrorFx),
					iv(dvars::r_mirrorViewmodel_flipReg),
					iv(dvars::r_mirrorViewmodel_flipFollow),
					iv(dvars::r_mirrorViewmodel_flipVSCF),
					iv(dvars::r_mirrorViewmodel_depthFix),
					iv(dvars::r_mirrorViewmodel_cullFix),
					iv(dvars::r_mirrorViewmodel_rttBlend),
					iv(dvars::r_mirrorViewmodel_rttTonemapInject)), 0);
			}
			game::Com_PrintMessage(0, utils::va(
				"[hudlog] f=%u SUMMARY c7=%d armed=%d reject=%d fire=%d beg=%d comp=%d "
				"dhp=%d final=%d hud_act=%d late_dhp=%d rt=%d "
				"inj_fail=%d early_comp=%d comp_in_hud=%d draws_in_hud=%d mtx64_in_hud=%d "
				"dhp_n=%d bsg=%d esg=%d inj=%d/%d follow_end=%d pass_end=%d prev_dhp=%d "
				"esc=%d\n",
				s_hudlog_frame,
				mirror_hud::g_c7_match_this_frame,
				mirror_hud::g_c7_armed_this_frame,
				mirror_hud::g_c7_rejected_this_frame,
				mirror_hud::g_alphatest_fires_this_frame,
				mirror_hud::g_begin_calls_this_frame,
				mirror_hud::g_composite_calls_this_frame,
				(int)mirror_rtt::g_dhp_seen_this_frame,
				(int)mirror_rtt::g_final_composite_done_this_frame,
				(int)mirror_hud::g_active,
				mirror_hud::g_late_dhp_this_frame,
				mirror_hud::g_rt_redirects_this_frame,
				mirror_hud::g_inj_fail_this_frame,
				mirror_hud::g_early_comp_this_frame,
				mirror_hud::g_comp_during_hud_this_frame,
				mirror_hud::g_draws_during_hud_this_frame,
				mirror_hud::g_mtx_flipreg_during_hud_this_frame,
				mirror_rtt::g_dhp_count_this_frame,
				mirror_rtt::g_begin_seg_count_this_frame,
				mirror_rtt::g_end_seg_count_this_frame,
				mirror_rtt::g_inject_ok_count_this_frame,
				mirror_rtt::g_inject_calls_this_frame,
				_renderer::mirror_vscf_follow_remaining,
				(int)mirror_rtt::g_pass_active,
				(int)mirror_rtt::g_dhp_seen_prev_frame,
				mirror_hud::g_shader_escapes_this_frame),   // v35.15
				0);
		}

		if (_renderer::mirror_dump_frames_remaining > 0)
		{
			// v35.1: log HUD-RTT diagnostic counters at end-of-frame so the
			// dump shows whether PSCF detection fired, whether begin_capture
			// ran, whether the engine rebound the RT mid-pass, and whether
			// composite executed. Helps localize HUD mirroring failures.
			_renderer::mirror_dump_write(
				"  HUD: pscf_hits=%d alphatest_fires=%d begin_calls=%d rt_redirects=%d composite_calls=%d\n",
				mirror_hud::g_pscf_hits_this_frame,
				mirror_hud::g_alphatest_fires_this_frame,
				mirror_hud::g_begin_calls_this_frame,
				mirror_hud::g_rt_redirects_this_frame,
				mirror_hud::g_composite_calls_this_frame);
			_renderer::mirror_dump_write("\n=== end of frame %d (EndScene) ===\n",
				_renderer::mirror_dump_frame_counter);
			_renderer::mirror_dump_frame_counter++;
			_renderer::mirror_dump_frames_remaining--;
			// v35.1: reset HUD-RTT per-frame counters so each frame in the
			// dump shows its own values.
			mirror_hud::g_pscf_hits_this_frame       = 0;
			mirror_hud::g_begin_calls_this_frame     = 0;
			mirror_hud::g_rt_redirects_this_frame    = 0;
			mirror_hud::g_composite_calls_this_frame = 0;
			mirror_hud::g_alphatest_fires_this_frame = 0;
			if (_renderer::mirror_dump_frames_remaining == 0)
			{
				_renderer::mirror_dump_close();
			}
		}

		// v32: r_fullMirror == 2 path: a single horizontal flip of the
		// entire final frame (including HUD). EndScene runs after the
		// engine has drawn the HUD, so flipping the BB here mirrors
		// world + gun + HUD as one. Brute-force option for video editing.
		{
			const int full_mirror_eos = dvars::r_fullMirror
				? dvars::r_fullMirror->current.integer : 0;
			if (full_mirror_eos == 2) mirror_rtt::do_fullscreen_flip(m_pIDirect3DDevice9);
		}

		// v33 (ported from cod4mirror): clear our off-screen RTT
		// depth-stencil to far at end of every frame. ReShade's
		// Generic Depth addon scans D3D9 CreateDepthStencilSurface
		// objects and may auto-pick our RTT g_depth instead of the
		// engine main DSV; that buffer holds the original right-side
		// (non-mirrored) gun and would produce a phantom MXAO
		// silhouette on the floor when the camera tilts down.
		// Clearing here makes the pick produce no AO.
		if (mirror_rtt::g_depth
			&& dvars::r_mirrorViewmodel_clearRttDepth
			&& dvars::r_mirrorViewmodel_clearRttDepth->current.integer != 0)
		{
			IDirect3DSurface9* prev_color = nullptr;
			IDirect3DSurface9* prev_depth = nullptr;
			m_pIDirect3DDevice9->GetRenderTarget(0, &prev_color);
			m_pIDirect3DDevice9->GetDepthStencilSurface(&prev_depth);
			// Clear() needs SOME render-target bound. g_color matches
			// g_depth's resolution and is already a render-target,
			// COLORWRITE=0 limits the clear to depth/stencil only.
			if (mirror_rtt::g_color) m_pIDirect3DDevice9->SetRenderTarget(0, mirror_rtt::g_color);
			m_pIDirect3DDevice9->SetDepthStencilSurface(mirror_rtt::g_depth);
			m_pIDirect3DDevice9->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
				0, 1.0f, 0);
			if (prev_color) { m_pIDirect3DDevice9->SetRenderTarget(0, prev_color); prev_color->Release(); }
			if (prev_depth) { m_pIDirect3DDevice9->SetDepthStencilSurface(prev_depth); prev_depth->Release(); }
			else            { m_pIDirect3DDevice9->SetDepthStencilSurface(nullptr); }
		}

		return m_pIDirect3DDevice9->EndScene();
	}

	HRESULT d3d9ex::D3D9Device::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
	{
		return m_pIDirect3DDevice9->Clear(Count, pRects, Flags, Color, Z, Stencil);
	}

	HRESULT d3d9ex::D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		return m_pIDirect3DDevice9->SetTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix)
	{
		return m_pIDirect3DDevice9->GetTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		return m_pIDirect3DDevice9->MultiplyTransform(State, pMatrix);
	}

	HRESULT d3d9ex::D3D9Device::SetViewport(CONST D3DVIEWPORT9* pViewport)
	{
		return m_pIDirect3DDevice9->SetViewport(pViewport);
	}

	HRESULT d3d9ex::D3D9Device::GetViewport(D3DVIEWPORT9* pViewport)
	{
		return m_pIDirect3DDevice9->GetViewport(pViewport);
	}

	HRESULT d3d9ex::D3D9Device::SetMaterial(CONST D3DMATERIAL9* pMaterial)
	{
		return m_pIDirect3DDevice9->SetMaterial(pMaterial);
	}

	HRESULT d3d9ex::D3D9Device::GetMaterial(D3DMATERIAL9* pMaterial)
	{
		return m_pIDirect3DDevice9->GetMaterial(pMaterial);
	}

	HRESULT d3d9ex::D3D9Device::SetLight(DWORD Index, CONST D3DLIGHT9* pLight)
	{
		return m_pIDirect3DDevice9->SetLight(Index, pLight);
	}

	HRESULT d3d9ex::D3D9Device::GetLight(DWORD Index, D3DLIGHT9* pLight)
	{
		return m_pIDirect3DDevice9->GetLight(Index, pLight);
	}

	HRESULT d3d9ex::D3D9Device::LightEnable(DWORD Index, BOOL Enable)
	{
		return m_pIDirect3DDevice9->LightEnable(Index, Enable);
	}

	HRESULT d3d9ex::D3D9Device::GetLightEnable(DWORD Index, BOOL* pEnable)
	{
		return m_pIDirect3DDevice9->GetLightEnable(Index, pEnable);
	}

	HRESULT d3d9ex::D3D9Device::SetClipPlane(DWORD Index, CONST float* pPlane)
	{
		return m_pIDirect3DDevice9->SetClipPlane(Index, pPlane);
	}

	HRESULT d3d9ex::D3D9Device::GetClipPlane(DWORD Index, float* pPlane)
	{
		return m_pIDirect3DDevice9->GetClipPlane(Index, pPlane);
	}

	HRESULT d3d9ex::D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
	{
		// v35.2: HUD-RTT capture fire. ALPHATESTENABLE=TRUE is the strong
		// HUD-start signal in iw3 - post-FX quads (tonemap, color grade,
		// additive glow) all run with alpha-test disabled, while real HUD
		// elements (text, ammo bar, crosshair, compass) toggle alpha-test
		// on. Firing begin_capture HERE rather than after the first draw
		// post-PSCF c7 keeps post-FX out of HUD-RTT, so composite()
		// only mirrors HUD pixels (world+gun stay on the back-buffer).
		//
		// v35.5: ALPHATESTENABLE=TRUE alone is NOT sufficient. During
		// damage-flash frames the engine emits a PSCF c7 fingerprint
		// match BEFORE world+gun rendering completes. The first
		// ALPHATESTENABLE=TRUE following that early fingerprint can be
		// a transparency pass during world rendering (e.g. damage-overlay
		// red gradient), and firing begin_capture there pulls the rest
		// of the world+gun+post-FX into HUD-RTT - the composite then
		// mirrors the entire scene instead of just the HUD (user-visible
		// symptom: whole image flips horizontally for ~2 seconds during
		// each grenade explosion, observed in v35.0..v35.4).
		//
		// Add a gate: only fire begin_capture when the gun-RTT pass has
		// already been resolved this frame (mirror_rtt::final_composite
		// has run, latching g_final_composite_done_this_frame). When
		// r_mirrorViewmodel_rtt is disabled there is no gun pass to
		// gate on, so treat the gate as always open in that case. This
		// rejects all early-c7 false triggers during world rendering;
		// the real post-FX c7 fingerprint fires AFTER the gun pass, so
		// the gate has been opened by the time the real HUD pass begins.
		// v35.8 firing gate. Arming has already been gated on
		// mirror_rtt::g_dhp_seen_this_frame at the PSCF c7 detection site,
		// so by the time g_capture_armed=true here the c7 fingerprint
		// fired AFTER at least one dhp upload (i.e. AFTER the gun pass
		// began). This rules out the early/false damage-flash c7 entirely.
		// On the firing side just fire on the next ALPHATESTENABLE=TRUE,
		// which by construction is the start of the real HUD pass
		// (post-FX quads in iw3 run with alpha-test disabled).
		if (State == D3DRS_ALPHATESTENABLE && Value
			&& mirror_hud::g_capture_armed && !mirror_hud::g_active)
		{
			mirror_hud::g_capture_armed = false;
			++mirror_hud::g_alphatest_fires_this_frame;
			mirror_hud::begin_capture(m_pIDirect3DDevice9);
			// v35.8.1 diagnostic: log begin_capture fire (HUD-RTT capture starting).
			if (hudlog_level() >= 2)
			{
				game::Com_PrintMessage(0, utils::va(
					"[hudlog] f=%u fire begin_capture (alpha_fire=%d beg=%d)\n",
					s_hudlog_frame,
					mirror_hud::g_alphatest_fires_this_frame,
					mirror_hud::g_begin_calls_this_frame), 0);
			}
		}
		// v35.20: SetRenderState override removed -- v35.19 log showed
		// ablend_ovr=0 every frame because the engine sets SRCBLENDALPHA
		// / DESTBLENDALPHA via a state block (CreateStateBlock + Apply)
		// rather than individual SetRenderState calls. Override moved
		// into Draw[Indexed]Primitive to guarantee it fires immediately
		// before the outline-shader draw regardless of how the values
		// got onto the device.
		const DWORD original_value = Value;
		bool swapped = false;

		// r_mirrorViewmodel: when the viewmodel projection is horizontally flipped,
		// screen-space winding order is reversed. Invert CULLMODE while
		// mirror_viewmodel_active so front faces stay visible. Gated by
		// r_mirrorViewmodel_cullFix so we can A/B test whether cull swap is the problem.
		if (State == D3DRS_CULLMODE)
		{
			const int log_level = dvars::r_mirrorViewmodel_log
				? dvars::r_mirrorViewmodel_log->current.integer : 0;
			const int cull_mode = dvars::r_mirrorViewmodel_cullFix
				? dvars::r_mirrorViewmodel_cullFix->current.integer : 0;

			if (_renderer::mirror_viewmodel_active && cull_mode != 0)
			{
				switch (cull_mode)
				{
				case 1:
					if (Value == D3DCULL_CW)       { Value = D3DCULL_CCW; swapped = true; }
					else if (Value == D3DCULL_CCW) { Value = D3DCULL_CW;  swapped = true; }
					break;
				case 2: if (Value != D3DCULL_CCW)  { Value = D3DCULL_CCW;  swapped = true; } break;
				case 3: if (Value != D3DCULL_CW)   { Value = D3DCULL_CW;   swapped = true; } break;
				case 4: if (Value != D3DCULL_NONE) { Value = D3DCULL_NONE; swapped = true; } break;
				}
			}

			if (log_level >= 2)
			{
				game::Com_PrintMessage(0, utils::va(
					"[mirror] CULLMODE: in=%u out=%u active=%d mode=%d swapped=%d\n",
					original_value, Value, (int)_renderer::mirror_viewmodel_active,
					cull_mode, (int)swapped), 0);
			}
		}

		// r_mirrorViewmodel dump: log every SetRenderState call with its decoded name.
		if (_renderer::mirror_dump_active())
		{
			mirror_dump_inc_rs();
			_renderer::mirror_dump_write(
				"  RS  %-28s (%3u) = %10u  vm_active=%d  swapped=%d\n",
				_renderer::mirror_dump_renderstate_name((unsigned)State),
				(unsigned)State, (unsigned)Value,
				(int)_renderer::mirror_viewmodel_active, (int)swapped);
		}

		return m_pIDirect3DDevice9->SetRenderState(State, Value);
	}

	HRESULT d3d9ex::D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
	{
		return m_pIDirect3DDevice9->GetRenderState(State, pValue);
	}

	HRESULT d3d9ex::D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB)
	{
		return m_pIDirect3DDevice9->CreateStateBlock(Type, ppSB);
	}

	HRESULT d3d9ex::D3D9Device::BeginStateBlock()
	{
		return m_pIDirect3DDevice9->BeginStateBlock();
	}

	HRESULT d3d9ex::D3D9Device::EndStateBlock(IDirect3DStateBlock9** ppSB)
	{
		return m_pIDirect3DDevice9->EndStateBlock(ppSB);
	}

	HRESULT d3d9ex::D3D9Device::SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus)
	{
		return m_pIDirect3DDevice9->SetClipStatus(pClipStatus);
	}

	HRESULT d3d9ex::D3D9Device::GetClipStatus(D3DCLIPSTATUS9* pClipStatus)
	{
		return m_pIDirect3DDevice9->GetClipStatus(pClipStatus);
	}

	HRESULT d3d9ex::D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
	{
		return m_pIDirect3DDevice9->GetTexture(Stage, ppTexture);
	}

	HRESULT d3d9ex::D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture)
	{
		// v35.15: classify stage-0 texture as 'big RT' (e.g. scene RT used
		// by the damage/death post-FX pass). Drives the narrowed shader-
		// escape in Draw[Indexed]Primitive. Normal HUD textures are
		// DXT5 D3DUSAGE_DYNAMIC (0x200) so this stays false during HUD.
		if (Stage == 0)
		{
			bool is_big_rt = false;
			if (pTexture && pTexture->GetType() == D3DRTYPE_TEXTURE)
			{
				IDirect3DTexture9* tex2d = static_cast<IDirect3DTexture9*>(pTexture);
				D3DSURFACE_DESC sd = {};
				if (SUCCEEDED(tex2d->GetLevelDesc(0, &sd))
					&& sd.Width >= 512u
					&& (sd.Usage & D3DUSAGE_RENDERTARGET) != 0)
				{
					is_big_rt = true;
				}
			}
			mirror_hud::g_last_tex0_is_big_rt = is_big_rt;
		}
		return m_pIDirect3DDevice9->SetTexture(Stage, pTexture);
	}

	HRESULT d3d9ex::D3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
	{
		return m_pIDirect3DDevice9->GetTextureStageState(Stage, Type, pValue);
	}

	HRESULT d3d9ex::D3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
	{
		return m_pIDirect3DDevice9->SetTextureStageState(Stage, Type, Value);
	}

	HRESULT d3d9ex::D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
	{
		return m_pIDirect3DDevice9->GetSamplerState(Sampler, Type, pValue);
	}

	HRESULT d3d9ex::D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
	{
		return m_pIDirect3DDevice9->SetSamplerState(Sampler, Type, Value);
	}

	HRESULT d3d9ex::D3D9Device::ValidateDevice(DWORD* pNumPasses)
	{
		return m_pIDirect3DDevice9->ValidateDevice(pNumPasses);
	}

	HRESULT d3d9ex::D3D9Device::SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries)
	{
		return m_pIDirect3DDevice9->SetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT d3d9ex::D3D9Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries)
	{
		return m_pIDirect3DDevice9->GetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT d3d9ex::D3D9Device::SetCurrentTexturePalette(UINT PaletteNumber)
	{
		return m_pIDirect3DDevice9->SetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT d3d9ex::D3D9Device::GetCurrentTexturePalette(UINT *PaletteNumber)
	{
		return m_pIDirect3DDevice9->GetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT d3d9ex::D3D9Device::SetScissorRect(CONST RECT* pRect)
	{
		return m_pIDirect3DDevice9->SetScissorRect(pRect);
	}

	HRESULT d3d9ex::D3D9Device::GetScissorRect(RECT* pRect)
	{
		return m_pIDirect3DDevice9->GetScissorRect(pRect);
	}

	HRESULT d3d9ex::D3D9Device::SetSoftwareVertexProcessing(BOOL bSoftware)
	{
		return m_pIDirect3DDevice9->SetSoftwareVertexProcessing(bSoftware);
	}

	BOOL d3d9ex::D3D9Device::GetSoftwareVertexProcessing()
	{
		return m_pIDirect3DDevice9->GetSoftwareVertexProcessing();
	}

	HRESULT d3d9ex::D3D9Device::SetNPatchMode(float nSegments)
	{
		return m_pIDirect3DDevice9->SetNPatchMode(nSegments);
	}

	float d3d9ex::D3D9Device::GetNPatchMode()
	{
		return m_pIDirect3DDevice9->GetNPatchMode();
	}

	HRESULT d3d9ex::D3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
	{
		// v35.10 diagnostic: count draws emitted while HUD-RTT is bound.
		// In normal frames the HUD pass after begin_capture only draws
		// 2D HUD elements; in damage frames a higher count or new VSCF
		// matrix uploads at flipReg would point at a stealth gun render.
		if (mirror_hud::g_active) {
			++mirror_hud::g_draws_during_hud_this_frame;
		}
		// v35.15: narrowed escape -- only redirect draws that sample a
		// large RENDERTARGET texture at stage 0 (the post-FX scene RT).
		// v35.14 used (vs_nonnull || ps_nonnull) which also matched HUD
		// draws in this engine (shader set before begin_capture), so the
		// entire HUD was bypassed -> HUD mirror broke. The RT-texture
		// signal is exclusive to damage/death post-FX in v35.13 log.
		const bool v35_14_escape_dp =
			mirror_hud::g_active
			&& mirror_hud::g_last_tex0_is_big_rt
			&& mirror_hud::g_saved_color;
		if (v35_14_escape_dp) {
			++mirror_hud::g_shader_escapes_this_frame;
			if (!mirror_hud::g_logged_first_shader_escape_this_frame && hudlog_level() >= 2) {
				mirror_hud::g_logged_first_shader_escape_this_frame = true;
				game::Com_PrintMessage(0, utils::va(
					"[hudlog] f=%u shader_escape kind=dp prim=%u tex0_rt=1\n",
					s_hudlog_frame, PrimitiveCount), 0);
			}
			m_pIDirect3DDevice9->SetRenderTarget(0, mirror_hud::g_saved_color);
			if (mirror_hud::g_saved_depth)
				m_pIDirect3DDevice9->SetDepthStencilSurface(mirror_hud::g_saved_depth);
		}
		if (_renderer::mirror_dump_active())
		{
			mirror_dump_inc_draw();
			_renderer::mirror_dump_write(
				"  DRAW raw prim=%u  follow=%d\n",
				PrimitiveCount, _renderer::mirror_vscf_follow_remaining);
		}
		HRESULT hr = m_pIDirect3DDevice9->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		if (v35_14_escape_dp) {
			m_pIDirect3DDevice9->SetRenderTarget(0, mirror_hud::g_color);
			m_pIDirect3DDevice9->SetDepthStencilSurface(nullptr);
		}
		if (mirror_rtt::g_pending_early_composite)
		{
			mirror_rtt::g_pending_early_composite = false;
			if (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment)
			{
				if (mirror_rtt::g_in_segment) mirror_rtt::end_segment(m_pIDirect3DDevice9);
				++mirror_hud::g_early_comp_this_frame; // v35.10
				if (mirror_hud::g_active) ++mirror_hud::g_comp_during_hud_this_frame; // v35.10
				mirror_rtt::final_composite(m_pIDirect3DDevice9);
			}
		}
		// v32: full-screen flip request from PSCF c7 fingerprint
		// (r_fullMirror == 1). Fires AFTER any early-composite so it
		// captures world + already-composited gun.
		if (mirror_rtt::g_pending_fullmirror_flip)
		{
			mirror_rtt::g_pending_fullmirror_flip = false;
			mirror_rtt::do_fullscreen_flip(m_pIDirect3DDevice9);
		}
		// v35.2: HUD-RTT capture is no longer fired from DrawPrimitive.
		// PSCF c7 only ARMS capture; the actual fire is deferred to the
		// first SetRenderState(ALPHATESTENABLE, TRUE) so post-FX quads
		// (color grade, additive glow) that come after the tonemap-output
		// draw still land on the engine's RT, not HUD-RTT.
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
	{
		if (mirror_hud::g_active) {
			++mirror_hud::g_draws_during_hud_this_frame; // v35.10
		}
		// v35.15: narrowed escape (see DrawPrimitive comment). Only
		// redirect draws that sample a large RENDERTARGET texture at
		// stage 0 -- the post-FX scene RT signature.
		const bool v35_14_escape_dip =
			mirror_hud::g_active
			&& mirror_hud::g_last_tex0_is_big_rt
			&& mirror_hud::g_saved_color;
		if (v35_14_escape_dip) {
			++mirror_hud::g_shader_escapes_this_frame;
			if (!mirror_hud::g_logged_first_shader_escape_this_frame && hudlog_level() >= 2) {
				mirror_hud::g_logged_first_shader_escape_this_frame = true;
				game::Com_PrintMessage(0, utils::va(
					"[hudlog] f=%u shader_escape kind=dip prim=%u nverts=%u tex0_rt=1\n",
					s_hudlog_frame, primCount, NumVertices), 0);
			}
			m_pIDirect3DDevice9->SetRenderTarget(0, mirror_hud::g_saved_color);
			if (mirror_hud::g_saved_depth)
				m_pIDirect3DDevice9->SetDepthStencilSurface(mirror_hud::g_saved_depth);
		}
		if (_renderer::mirror_dump_active())
		{
			mirror_dump_inc_draw();
			_renderer::mirror_dump_write(
				"  DRAW idx prim=%u nverts=%u  follow=%d\n",
				primCount, NumVertices, _renderer::mirror_vscf_follow_remaining);
		}
		HRESULT hr = m_pIDirect3DDevice9->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		if (v35_14_escape_dip) {
			m_pIDirect3DDevice9->SetRenderTarget(0, mirror_hud::g_color);
			m_pIDirect3DDevice9->SetDepthStencilSurface(nullptr);
		}
		// v22: composite the mirrored viewmodel right AFTER the engine's
		// final tonemap/output draw (the first draw following the PSCF c7
		// fingerprint). The pending flag was set by SetPixelShaderConstantF.
		if (mirror_rtt::g_pending_early_composite)
		{
			mirror_rtt::g_pending_early_composite = false;
			if (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment)
			{
				if (mirror_rtt::g_in_segment) mirror_rtt::end_segment(m_pIDirect3DDevice9);
				++mirror_hud::g_early_comp_this_frame; // v35.10
				if (mirror_hud::g_active) ++mirror_hud::g_comp_during_hud_this_frame; // v35.10
				mirror_rtt::final_composite(m_pIDirect3DDevice9);
			}
		}
		// v32: full-screen flip request from PSCF c7 fingerprint
		// (r_fullMirror == 1). Fires AFTER any early-composite so it
		// captures world + already-composited gun.
		if (mirror_rtt::g_pending_fullmirror_flip)
		{
			mirror_rtt::g_pending_fullmirror_flip = false;
			mirror_rtt::do_fullscreen_flip(m_pIDirect3DDevice9);
		}
		// v35.2: HUD-RTT capture is no longer fired from DrawIndexedPrimitive.
		// PSCF c7 only ARMS capture; the actual fire is deferred to the
		// first SetRenderState(ALPHATESTENABLE, TRUE) so post-FX quads
		// (color grade, additive glow) that come after the tonemap-output
		// draw still land on the engine's RT, not HUD-RTT.
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
	{
		return m_pIDirect3DDevice9->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
	}

	HRESULT d3d9ex::D3D9Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
	{
		return m_pIDirect3DDevice9->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
	}

	HRESULT d3d9ex::D3D9Device::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags)
	{
		return m_pIDirect3DDevice9->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl)
	{
		return m_pIDirect3DDevice9->CreateVertexDeclaration(pVertexElements, ppDecl);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl)
	{
		return m_pIDirect3DDevice9->SetVertexDeclaration(pDecl);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl)
	{
		return m_pIDirect3DDevice9->GetVertexDeclaration(ppDecl);
	}

	HRESULT d3d9ex::D3D9Device::SetFVF(DWORD FVF)
	{
		return m_pIDirect3DDevice9->SetFVF(FVF);
	}

	HRESULT d3d9ex::D3D9Device::GetFVF(DWORD* pFVF)
	{
		return m_pIDirect3DDevice9->GetFVF(pFVF);
	}

	HRESULT d3d9ex::D3D9Device::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader)
	{
		return m_pIDirect3DDevice9->CreateVertexShader(pFunction, ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShader(IDirect3DVertexShader9* pShader)
	{
		return m_pIDirect3DDevice9->SetVertexShader(pShader);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShader(IDirect3DVertexShader9** ppShader)
	{
		return m_pIDirect3DDevice9->GetVertexShader(ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		// r_mirrorViewmodel v10: matrix-upload heuristic with selectable flip axis.
		// v8 negated only c0 register (first 4 floats). User dump confirmed flip
		// reached GPU during 5 gun draws but gun did NOT mirror visually.
		// Conclusion: matrix is stored column-major, so c0 register = column 0 of
		// matrix. Negating c0 scales vertex.x contribution, not clip.x output.
		//
		// v10 adds r_mirrorViewmodel_flipAxis:
		//   0 = row (v8)    - negate c0 register entirely (first row if row-major)
		//   1 = col (new)   - negate first element of each register (first row if
		//                     column-major = the clip.x output row). DEFAULT.
		//   2 = both        - belt-and-suspenders
		//   3 = full        - negate all 16 floats (diagnostic: if matrix IS used
		//                     by gun vs, the gun should visibly distort/vanish;
		//                     if no change, c0-c3 isn't used by gun vs at all)
		float local_mtx[16];
		const float* out_data = pConstantData;
		// is_mtx_at_zero detects c0-c3 matrix uploads (used for dhp/stdp arming logic only)
		const bool is_mtx_at_zero = (pConstantData && StartRegister == 0 && Vector4fCount == 4);
		float c23 = 0.0f;
		if (is_mtx_at_zero) { c23 = pConstantData[11]; } // c2[3]
		const bool is_depth_hack_proj = is_mtx_at_zero && (c23 < -0.02f && c23 > -0.50f);
		const bool is_std_proj        = is_mtx_at_zero && (c23 < -1.00f);

		const int flipVSCF = dvars::r_mirrorViewmodel_flipVSCF
			? dvars::r_mirrorViewmodel_flipVSCF->current.integer : 0;
		const int flipFollow = dvars::r_mirrorViewmodel_flipFollow
			? dvars::r_mirrorViewmodel_flipFollow->current.integer : 0;
		const int flipAxis = dvars::r_mirrorViewmodel_flipAxis
			? dvars::r_mirrorViewmodel_flipAxis->current.integer : 4;
		const int flipReg = dvars::r_mirrorViewmodel_flipReg
			? dvars::r_mirrorViewmodel_flipReg->current.integer : 0;

		// Arm/disarm follow window from c0-c3 dhp/stdp detection. Independent of flipReg.
		if (is_depth_hack_proj)
		{
			_renderer::mirror_vscf_follow_remaining = flipFollow;
			// v34.6: latch the gun-seen flag for the entire present-cycle.
			// Used by r_hudMirror gate; reset only on Present.
			_renderer::gun_seen_this_present = true;
			// v35.8: latch the per-frame dhp-seen flag. Detected here
			// regardless of r_mirrorViewmodel_rtt (the rtt-gated
			// begin_segment block below only fires when rtt=1, but the
			// dhp matrix shape itself is uploaded by the engine for the
			// gun pass even when rtt=0). Reset in BeginScene. Used by
			// mirror_hud's PSCF c7 arming gate to reject the early/false
			// damage-flash c7 fingerprint that fires before any dhp
			// upload; only post-gun (real post-FX) c7 fingerprints arm.
			const bool dhp_was_seen = mirror_rtt::g_dhp_seen_this_frame;
			mirror_rtt::g_dhp_seen_this_frame = true;
			++mirror_rtt::g_dhp_count_this_frame; // v35.11

			// v35.8.1 diagnostic: log only on FIRST dhp upload of the frame
			// (subsequent dhp uploads in the same frame would spam the log).
			if (!dhp_was_seen && hudlog_level() >= 2)
			{
				game::Com_PrintMessage(0, utils::va(
					"[hudlog] f=%u dhp_upload (first) pass_active=%d in_seg=%d final_done=%d c7_match_so_far=%d c7_rejected_so_far=%d\n",
					s_hudlog_frame,
					(int)mirror_rtt::g_pass_active,
					(int)mirror_rtt::g_in_segment,
					(int)mirror_rtt::g_final_composite_done_this_frame,
					mirror_hud::g_c7_match_this_frame,
					mirror_hud::g_c7_rejected_this_frame), 0);
				// v35.12: dump first dhp matrix diagonal + first column to detect
				// horizontal flip (negative c0[0] would mean engine pre-flipped
				// the gun matrix, which combined with our inject UV-flip would
				// produce un-mirrored gun on BB during damage flinch).
				game::Com_PrintMessage(0, utils::va(
					"[hudlog] f=%u dhp_mtx c0=(%.4f %.4f %.4f %.4f) c1[1]=%.4f c2[2]=%.4f c2[3]=%.4f c3=(%.4f %.4f %.4f %.4f)\n",
					s_hudlog_frame,
					pConstantData[0],  pConstantData[1],  pConstantData[2],  pConstantData[3],
					pConstantData[5],
					pConstantData[10],
					pConstantData[11],
					pConstantData[12], pConstantData[13], pConstantData[14], pConstantData[15]
				), 0);
			}

			// v35.9 diagnostic: dhp uploads that arrive AFTER mirror_hud
			// HUD-RTT capture has started. These are the "second gun pass"
			// in damage frames that drive the EndScene double-flip bug; the
			// EndScene fix below covers them. Counter is reported in SUMMARY.
			if (mirror_hud::g_active)
			{
				++mirror_hud::g_late_dhp_this_frame;
				if (hudlog_level() >= 2)
				{
					game::Com_PrintMessage(0, utils::va(
						"[hudlog] f=%u dhp_upload (LATE, after begin_capture) late_dhp=%d\n",
						s_hudlog_frame,
						mirror_hud::g_late_dhp_this_frame), 0);
				}
			}
		}
		else if (is_std_proj)
		{
			_renderer::mirror_vscf_follow_remaining = 0;
		}

		// v15: render-to-texture mirror, segment-aware.
		// dhp upload: bind off-screen color+depth (clear once per frame).
		// stdp upload: switch back to engine RT (no composite). Final composite
		// happens once at EndScene.
		// v18 attempted to composite at first 2D-ortho upload (to render HUD on top
		// of the gun) but it fired during in-frame post-FX/stencil-shadow ortho
		// passes that share the HUD ortho signature, leaving the gun invisible.
		// Reverted to EndScene composite; HUD overlays the gun visually but the
		// gun is reliably visible everywhere.
		const int rtt_on = dvars::r_mirrorViewmodel_rtt
			? dvars::r_mirrorViewmodel_rtt->current.integer : 0;
		if (rtt_on)
		{
			if (is_depth_hack_proj)
			{
				mirror_rtt::begin_segment(m_pIDirect3DDevice9);
			}
			// v17: end the segment on ANY non-dhp 4-row c0-c3 matrix upload, not just
			// the world std-proj signature. The iron-sight reticle pass uses a third
			// dhp matrix and is followed by an identity matrix upload (2D HUD setup,
			// c2[3]=1.0) - that is neither dhp nor stdp, so v15-v16's stdp-only check
			// missed it and the segment stayed open across the entire 2D HUD pass.
			else if (is_mtx_at_zero && mirror_rtt::g_in_segment)
			{
				mirror_rtt::end_segment(m_pIDirect3DDevice9);
			}
		}

		// v35: r_hudMirror is now implemented via HUD-RTT capture (see
		// mirror_hud:: namespace). The old v34 VSCF projection-flip path
		// was removed because the engine shares one 2D-ortho matrix
		// across stencil/post-FX/HUD - flipping it broke post-FX too.

		// Apply flip if: this upload is a 4-row matrix at the configured flipReg AND we are in
		// a gun pass (dhp itself, or within follow window when flipVSCF==2).
		// When rtt is on, the off-screen render path replaces matrix-flip; disable it.
		const bool is_target_mtx = (pConstantData && Vector4fCount == 4 && (int)StartRegister == flipReg);
		// v35.10 diagnostic: count gun-matrix-shape uploads (4-row at flipReg)
		// that arrive while HUD-RTT capture is in progress. If non-zero in
		// damage frames, a stealth gun re-render is happening with the HUD
		// RT bound -- the matrix-flipped gun would land on HUD-RTT, then
		// composite() flips horizontally, double-flipping it (un-mirrored).
		if (is_target_mtx && mirror_hud::g_active)
			++mirror_hud::g_mtx_flipreg_during_hud_this_frame;
		bool apply_flip = false;
		if (is_target_mtx && flipVSCF != 0 && !rtt_on)
		{
			if (flipReg == 0)
			{
				// Targeting c0-c3: only flip the dhp upload itself (mode 1) or dhp+follow (mode 2).
				if (flipVSCF == 1 && is_depth_hack_proj) apply_flip = true;
				if (flipVSCF == 2 && (is_depth_hack_proj || _renderer::mirror_vscf_follow_remaining > 0))
					apply_flip = true;
			}
			else
			{
				// Targeting c4-c7 / c24-c27 / etc: only fires DURING the gun pass (follow window > 0).
				// These registers do not carry the dhp signature, so we rely on the follow window
				// to know we are in a gun draw block. flipVSCF mode is treated the same here.
				if (_renderer::mirror_vscf_follow_remaining > 0) apply_flip = true;
			}
		}

		if (apply_flip)
		{
			for (int i = 0; i < 16; ++i) local_mtx[i] = pConstantData[i];
			// flipAxis selects which subset of the 4x4 matrix to negate.
			//   0..3 = negate row N of register block (4 floats: local_mtx[N*4 .. N*4+3])
			//          - this is row N of matrix if storage is row-major
			//   4..7 = negate "col" N: local_mtx[N], local_mtx[N+4], local_mtx[N+8], local_mtx[N+12]
			//          - this is row N of matrix if storage is column-major (D3D9 default)
			//   8    = full: all 16 floats negated (clip.w flips sign -> gun clipped behind cam)
			//   9    = row 0 + col 0 simultaneously (v10 axis=2 belt-and-suspenders behavior)
			switch (flipAxis) {
			case 0: case 1: case 2: case 3: {
				const int base = flipAxis * 4;
				local_mtx[base+0] = -local_mtx[base+0];
				local_mtx[base+1] = -local_mtx[base+1];
				local_mtx[base+2] = -local_mtx[base+2];
				local_mtx[base+3] = -local_mtx[base+3];
			} break;
			case 4: case 5: case 6: case 7: {
				const int off = flipAxis - 4;
				local_mtx[off+0]  = -local_mtx[off+0];
				local_mtx[off+4]  = -local_mtx[off+4];
				local_mtx[off+8]  = -local_mtx[off+8];
				local_mtx[off+12] = -local_mtx[off+12];
			} break;
			case 8:
				for (int i = 0; i < 16; ++i) local_mtx[i] = -local_mtx[i];
				break;
			case 9:
				local_mtx[0]  = -local_mtx[0];
				local_mtx[1]  = -local_mtx[1];
				local_mtx[2]  = -local_mtx[2];
				local_mtx[3]  = -local_mtx[3];
				local_mtx[0]  = -local_mtx[0]; // double-negate first elem -> back to orig
				local_mtx[4]  = -local_mtx[4];
				local_mtx[8]  = -local_mtx[8];
				local_mtx[12] = -local_mtx[12];
				break;
			}
			out_data = local_mtx;
		}

		// Decay the follow window after applying. Don't decay on the dhp upload itself
		// (it just rearmed); decay on every other matrix upload while armed.
		if (is_mtx_at_zero && !is_depth_hack_proj && _renderer::mirror_vscf_follow_remaining > 0)
		{
			--_renderer::mirror_vscf_follow_remaining;
		}

		if (_renderer::mirror_dump_active() && pConstantData)
		{
			mirror_dump_inc_vscf();
			_renderer::mirror_dump_write(
				"  VSCF start=%u count=%u vm_active=%d flip=%d dhp=%d stdp=%d\n",
				StartRegister, Vector4fCount, (int)_renderer::mirror_viewmodel_active,
				(out_data != pConstantData) ? 1 : 0,
				(int)is_depth_hack_proj, (int)is_std_proj);
			const UINT rows = (Vector4fCount > 16) ? 16 : Vector4fCount;
			for (UINT i = 0; i < rows; ++i)
			{
				_renderer::mirror_dump_write(
					"    c%3u : % .6f  % .6f  % .6f  % .6f\n",
					StartRegister + i,
					out_data[i*4+0], out_data[i*4+1],
					out_data[i*4+2], out_data[i*4+3]);
			}
		}
		return m_pIDirect3DDevice9->SetVertexShaderConstantF(StartRegister, out_data, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		return m_pIDirect3DDevice9->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		return m_pIDirect3DDevice9->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		return m_pIDirect3DDevice9->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		return m_pIDirect3DDevice9->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		return m_pIDirect3DDevice9->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride)
	{
		return m_pIDirect3DDevice9->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
	}

	HRESULT d3d9ex::D3D9Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes, UINT* pStride)
	{
		return m_pIDirect3DDevice9->GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride);
	}

	HRESULT d3d9ex::D3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Divider)
	{
		return m_pIDirect3DDevice9->SetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT d3d9ex::D3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider)
	{
		return m_pIDirect3DDevice9->GetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT d3d9ex::D3D9Device::SetIndices(IDirect3DIndexBuffer9* pIndexData)
	{
		return m_pIDirect3DDevice9->SetIndices(pIndexData);
	}

	HRESULT d3d9ex::D3D9Device::GetIndices(IDirect3DIndexBuffer9** ppIndexData)
	{
		return m_pIDirect3DDevice9->GetIndices(ppIndexData);
	}

	HRESULT d3d9ex::D3D9Device::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
	{
		return m_pIDirect3DDevice9->CreatePixelShader(pFunction, ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShader(IDirect3DPixelShader9* pShader)
	{
		return m_pIDirect3DDevice9->SetPixelShader(pShader);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShader(IDirect3DPixelShader9** ppShader)
	{
		return m_pIDirect3DDevice9->GetPixelShader(ppShader);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		// Use real bad readptr check here, cause the query takes too long
		// TODO: Fix the actual error!
		if (IsBadReadPtr(pConstantData, Vector4fCount * 16))
		{
			//Logger::Print("Invalid shader constant array!\n");
			return D3DERR_INVALIDCALL;
		}

		if (_renderer::mirror_dump_active() && pConstantData)
		{
			mirror_dump_inc_pscf();
			_renderer::mirror_dump_write(
				"  PSCF start=%u count=%u vm_active=%d\n",
				StartRegister, Vector4fCount, (int)_renderer::mirror_viewmodel_active);
			const UINT rows = (Vector4fCount > 8) ? 8 : Vector4fCount;
			for (UINT i = 0; i < rows; ++i)
			{
				_renderer::mirror_dump_write(
					"    c%3u : % .6f  % .6f  % .6f  % .6f\n",
					StartRegister + i,
					pConstantData[i*4+0], pConstantData[i*4+1],
					pConstantData[i*4+2], pConstantData[i*4+3]);
			}
		}

		// v20: detect the engine's final post-FX/HUD-boundary pixel-shader
		// constant. Across maps and graphic configs the engine uploads a
		// PSCF c7 = (-0.066, -0.066, -0.066, 2.773585) exactly once per
		// frame, immediately before the first HUD ortho c0-c3 upload.
		// This is a far more reliable HUD-start signal than the 2D ortho
		// matrix shape that v18 tried to use - in-frame stencil-shadow and
		// post-FX passes share the ortho signature, but they do NOT share
		// these specific PSCF constants. Compositing here means the gun
		// is on the back-buffer before HUD draws, so the HUD overlays the
		// gun (timer, C4, ammo no longer hidden behind the mirrored view).
		// Falls back to the EndScene composite if this signal is absent
		// (e.g. the technique is bypassed by a graphics setting).
		const int rtt_on_pscf = dvars::r_mirrorViewmodel_rtt
			? dvars::r_mirrorViewmodel_rtt->current.integer : 0;
		const int rtt_early = dvars::r_mirrorViewmodel_rttEarlyComposite
			? dvars::r_mirrorViewmodel_rttEarlyComposite->current.integer : 1;
		if (rtt_on_pscf && rtt_early != 0 && pConstantData
			&& StartRegister == 7 && Vector4fCount >= 1
			&& (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment))
		{
			const float c70 = pConstantData[0];
			const float c71 = pConstantData[1];
			const float c72 = pConstantData[2];
			const float c73 = pConstantData[3];
			// v23/v37: generalized tonemap-shader fingerprint. The exact values
			// shift based on the engine's gamma/exposure/grading dvars (default
			// (-0.066,-0.066,-0.066, 2.773585); demo replay (-0.013772,...,1.222222);
			// r_desaturation=0 -> c73~13.24; r_contrast=2 -> c70~-0.766) but the
			// STRUCTURE is the same: c70..c72 equal and negative, c73 a positive
			// gamma-exponent. v37 widens bounds AND caches the pixel-shader
			// pointer on first structural match so extreme film tweak settings
			// (r_filmTweakBrightness/Contrast/Desaturation, r_contrast,
			// r_desaturation) no longer break detection.
			const bool is_pre_hud_signal = mirror_rtt::match_tonemap_signal(
				m_pIDirect3DDevice9, c70, c71, c72, c73);
			if (is_pre_hud_signal)
			{
				// v33 (ported from cod4mirror): rewrite main depth at the
				// gun pixels so post-process AO (ReShade MXAO/SSAO) does
				// not bleed wall shadows through the mirrored gun. Engine
				// main RT+DSV are still bound here, which is what the
				// depth-fix pass needs (it only writes Z, COLORWRITE=0).
				const int dfix_mode = dvars::r_mirrorViewmodel_depthFix
					? dvars::r_mirrorViewmodel_depthFix->current.integer : 0;
				if (dfix_mode > 0)
				{
					if (mirror_rtt::g_in_segment) mirror_rtt::end_segment(m_pIDirect3DDevice9);
					mirror_rtt::apply_main_depth_fix(m_pIDirect3DDevice9, dfix_mode);
				}

				const int tonemap_inject = dvars::r_mirrorViewmodel_rttTonemapInject
					? dvars::r_mirrorViewmodel_rttTonemapInject->current.integer : 1;
				if (tonemap_inject && mirror_rtt::inject_into_tonemap_source(m_pIDirect3DDevice9))
				{
					// v25: merge the mirrored gun into the engine's tonemap SOURCE
					// texture before the final fullscreen tonemap-output draw so the
					// gun receives the same film / contrast / color-grade curve as
					// the world.
				}
				else
				{
					// v22 fallback: do NOT composite here. The next DrawIndexedPrimitive
					// call is the engine's final tonemap/output pass that writes the
					// processed scene to the back-buffer (an opaque overwrite).
					// Compositing before it would let that pass paint over the gun;
					// instead set a pending flag so we composite AFTER that draw.
					if (mirror_rtt::g_in_segment) mirror_rtt::end_segment(m_pIDirect3DDevice9);
					mirror_rtt::g_pending_early_composite = true;
					// v35.10 diagnostic: inject failed -> fallback active
					++mirror_hud::g_inj_fail_this_frame;
				}
			}
		}

		// v32: detect the same PSCF c7 tonemap fingerprint INDEPENDENTLY
		// for r_fullMirror == 1. Sets a pending flag that fires after
		// the next draw (tonemap output) so the BB is mirrored before
		// HUD draws on top.
		{
			const int full_mirror = dvars::r_fullMirror
				? dvars::r_fullMirror->current.integer : 0;
			if (full_mirror == 1 && pConstantData && StartRegister == 7 && Vector4fCount >= 1)
			{
				const float c70 = pConstantData[0];
				const float c71 = pConstantData[1];
				const float c72 = pConstantData[2];
				const float c73 = pConstantData[3];
				// v37: widened bounds + PS-pointer cache fallback so r_fullMirror
				// also survives extreme r_contrast / r_desaturation values.
				const bool is_pre_hud_signal = mirror_rtt::match_tonemap_signal(
					m_pIDirect3DDevice9, c70, c71, c72, c73);
				if (is_pre_hud_signal) mirror_rtt::g_pending_fullmirror_flip = true;
			}
		}

		// v35: detect the same PSCF c7 tonemap fingerprint INDEPENDENTLY
		// for r_hudMirror. Sets a pending flag that fires after the next
		// draw (the engine's final tonemap-output draw); on that fire we
		// begin HUD-RTT capture so all subsequent HUD draws land off-screen.
		// Matches the r_fullMirror block above structurally so the gating
		// and fingerprint thresholds stay consistent across both paths.
		{
			const int hud_mirror_pscf = dvars::r_hudMirror
				? dvars::r_hudMirror->current.integer : 0;
			if (hud_mirror_pscf == 1 && pConstantData && StartRegister == 7 && Vector4fCount >= 1
				&& !mirror_hud::g_active)
			{
				const float c70 = pConstantData[0];
				const float c71 = pConstantData[1];
				const float c72 = pConstantData[2];
				const float c73 = pConstantData[3];
				// v37: widened bounds + PS-pointer cache fallback so r_hudMirror
				// also survives extreme r_contrast / r_desaturation values.
				const bool is_pre_hud_signal = mirror_rtt::match_tonemap_signal(
					m_pIDirect3DDevice9, c70, c71, c72, c73);
				// v35.8: gate arming on mirror_rtt::g_dhp_seen_this_frame.
				// In damage-flash frames the engine emits an early c7
				// match BEFORE the gun pass starts (during damage-overlay
				// setup). dhp_seen=false at that moment, so we reject the
				// arm and the subsequent ALPHATESTENABLE during world
				// rendering will not fire begin_capture (which would pull
				// world+gun+post-FX into HUD-RTT and produce the v35.0..v35.4
				// and v35.7 whole-scene flip during damage). Real post-FX c7
				// fires AFTER the gun pass with dhp_seen=true and arms
				// normally. pscf_hits_this_frame is incremented either way
				// for diagnostics so the dump shows total c7 fingerprint
				// matches even if some were rejected.
				if (is_pre_hud_signal) {
					++mirror_hud::g_pscf_hits_this_frame;
					++mirror_hud::g_c7_match_this_frame;
					// v35.12: arm if (a) gun pass already happened this frame
					// (v35.8 normal case), OR (b) previous frame also had no
					// gun pass (sustained death cam: 3rd-person view, no
					// viewmodel ever uploads dhp). Damage-flash early c7
					// (prev frame had gun, current dhp not yet seen) is still
					// rejected because the real post-FX c7 will arm shortly.
					const bool armed_now =
						mirror_rtt::g_dhp_seen_this_frame ||
						!mirror_rtt::g_dhp_seen_prev_frame;
					if (armed_now) {
						mirror_hud::g_capture_armed = true;
						++mirror_hud::g_c7_armed_this_frame;
					} else {
						++mirror_hud::g_c7_rejected_this_frame;
					}
					// v35.8.1 diagnostic: log every c7 fingerprint match with arming
					// gate outcome. "armed" = will fire begin_capture on next
					// ALPHATESTENABLE=TRUE; "rejected" = early/pre-gun c7 dropped
					// by v35.8 gate (HUD will NOT capture this c7).
					if (hudlog_level() >= 2)
					{
						game::Com_PrintMessage(0, utils::va(
							"[hudlog] f=%u c7_match c=(%.6f %.6f %.6f %.6f) dhp_seen=%d prev_dhp=%d -> %s\n",
							s_hudlog_frame, c70, c71, c72, c73,
							(int)mirror_rtt::g_dhp_seen_this_frame,
							(int)mirror_rtt::g_dhp_seen_prev_frame,
							armed_now ? "ARMED" : "REJECTED"), 0);
					}
				}
			}
		}

		return m_pIDirect3DDevice9->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		return m_pIDirect3DDevice9->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		return m_pIDirect3DDevice9->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		return m_pIDirect3DDevice9->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT d3d9ex::D3D9Device::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		return m_pIDirect3DDevice9->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		return m_pIDirect3DDevice9->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT d3d9ex::D3D9Device::DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo)
	{
		return m_pIDirect3DDevice9->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
	}

	HRESULT d3d9ex::D3D9Device::DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo)
	{
		return m_pIDirect3DDevice9->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
	}

	HRESULT d3d9ex::D3D9Device::DeletePatch(UINT Handle)
	{
		return m_pIDirect3DDevice9->DeletePatch(Handle);
	}

	HRESULT d3d9ex::D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
	{
		return m_pIDirect3DDevice9->CreateQuery(Type, ppQuery);
	}

#pragma endregion

#pragma region _D3D9

	HRESULT __stdcall d3d9ex::_d3d9::QueryInterface(REFIID riid, void** ppvObj)
	{
		*ppvObj = nullptr;

		HRESULT hRes = m_pIDirect3D9->QueryInterface(riid, ppvObj);

		if (hRes == NOERROR)
		{
			*ppvObj = this;
		}

		return hRes;
	}

	ULONG __stdcall d3d9ex::_d3d9::AddRef()
	{
		return m_pIDirect3D9->AddRef();
	}

	ULONG __stdcall d3d9ex::_d3d9::Release()
	{
		ULONG count = m_pIDirect3D9->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT __stdcall d3d9ex::_d3d9::RegisterSoftwareDevice(void* pInitializeFunction)
	{
		return m_pIDirect3D9->RegisterSoftwareDevice(pInitializeFunction);
	}

	UINT __stdcall d3d9ex::_d3d9::GetAdapterCount()
	{
		return m_pIDirect3D9->GetAdapterCount();
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
	{
		return m_pIDirect3D9->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
	}

	UINT __stdcall d3d9ex::_d3d9::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
	{
		return m_pIDirect3D9->GetAdapterModeCount(Adapter, Format);
	}

	HRESULT __stdcall d3d9ex::_d3d9::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9->EnumAdapterModes(Adapter, Format, Mode, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9->GetAdapterDisplayMode(Adapter, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceType(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
	{
		return m_pIDirect3D9->CheckDeviceType(iAdapter, DevType, DisplayFormat, BackBufferFormat, bWindowed);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
	{
		return m_pIDirect3D9->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
	{
		return m_pIDirect3D9->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
	{
		return m_pIDirect3D9->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
	{
		return m_pIDirect3D9->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
	{
		return m_pIDirect3D9->GetDeviceCaps(Adapter, DeviceType, pCaps);
	}

	HMONITOR __stdcall d3d9ex::_d3d9::GetAdapterMonitor(UINT Adapter)
	{
		return m_pIDirect3D9->GetAdapterMonitor(Adapter);
	}

	HRESULT __stdcall d3d9ex::_d3d9::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
	{
		HRESULT hres = m_pIDirect3D9->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
		
		*ppReturnedDeviceInterface = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
		game::glob::d3d9_device = *ppReturnedDeviceInterface;

		rtx::on_device_creation();

		return hres;
	}


#pragma endregion

#pragma region _D3D9Ex

	HRESULT __stdcall d3d9ex::_d3d9ex::QueryInterface(REFIID riid, void** ppvObj)
	{
		*ppvObj = nullptr;

		HRESULT hRes = m_pIDirect3D9Ex->QueryInterface(riid, ppvObj);

		if (hRes == NOERROR)
		{
			*ppvObj = this;
		}

		return hRes;
	}

	ULONG __stdcall d3d9ex::_d3d9ex::AddRef()
	{
		return m_pIDirect3D9Ex->AddRef();
	}

	ULONG __stdcall d3d9ex::_d3d9ex::Release()
	{
		ULONG count = m_pIDirect3D9Ex->Release();
		if (!count) delete this;
		return count;
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::RegisterSoftwareDevice(void* pInitializeFunction)
	{
		return m_pIDirect3D9Ex->RegisterSoftwareDevice(pInitializeFunction);
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterCount()
	{
		return m_pIDirect3D9Ex->GetAdapterCount();
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
	{
		return m_pIDirect3D9Ex->GetAdapterIdentifier(Adapter, Flags, pIdentifier);
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
	{
		return m_pIDirect3D9Ex->GetAdapterModeCount(Adapter, Format);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9Ex->EnumAdapterModes(Adapter, Format, Mode, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
	{
		return m_pIDirect3D9Ex->GetAdapterDisplayMode(Adapter, pMode);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceType(UINT iAdapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
	{
		return m_pIDirect3D9Ex->CheckDeviceType(iAdapter, DevType, DisplayFormat, BackBufferFormat, bWindowed);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
	{
		return m_pIDirect3D9Ex->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
	{
		return m_pIDirect3D9Ex->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
	{
		return m_pIDirect3D9Ex->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
	{
		return m_pIDirect3D9Ex->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
	{
		return m_pIDirect3D9Ex->GetDeviceCaps(Adapter, DeviceType, pCaps);
	}

	HMONITOR __stdcall d3d9ex::_d3d9ex::GetAdapterMonitor(UINT Adapter)
	{
		return m_pIDirect3D9Ex->GetAdapterMonitor(Adapter);
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface)
	{
		HRESULT hres = m_pIDirect3D9Ex->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
		
		*ppReturnedDeviceInterface = new d3d9ex::D3D9Device(*ppReturnedDeviceInterface);
		game::glob::d3d9_device = *ppReturnedDeviceInterface;

		rtx::on_device_creation();

		return hres;
	}

	UINT __stdcall d3d9ex::_d3d9ex::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter)
	{
		return (m_pIDirect3D9Ex->GetAdapterModeCountEx(Adapter, pFilter));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::EnumAdapterModesEx(UINT Adapter, const D3DDISPLAYMODEFILTER* pFilter, UINT Mode, D3DDISPLAYMODEEX* pMode)
	{
		return (m_pIDirect3D9Ex->EnumAdapterModesEx(Adapter, pFilter, Mode, pMode));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
	{
		return (m_pIDirect3D9Ex->GetAdapterDisplayModeEx(Adapter, pMode, pRotation));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode, IDirect3DDevice9Ex** ppReturnedDeviceInterface)
	{
		return (m_pIDirect3D9Ex->CreateDeviceEx(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface));
	}

	HRESULT __stdcall d3d9ex::_d3d9ex::GetAdapterLUID(UINT Adapter, LUID* pLUID)
	{
		return (m_pIDirect3D9Ex->GetAdapterLUID(Adapter, pLUID));
	}
#pragma endregion

	IDirect3D9* __stdcall d3d9ex::direct3d_create9_stub(UINT sdk)
	{
		if (dvars::r_d3d9ex && dvars::r_d3d9ex->current.enabled)
		{
			IDirect3D9Ex* d3d9ex = nullptr;
			
			if (SUCCEEDED(Direct3DCreate9Ex(sdk, &d3d9ex))) 
			{
				return (new d3d9ex::_d3d9ex(d3d9ex));
			}

			game::Com_PrintMessage(0, "Direct3D9Ex failed to initialize. Defaulting to Direct3D9.\n", 0);
		}

		return (new d3d9ex::_d3d9(Direct3DCreate9(sdk)));
	}

	d3d9ex::d3d9ex()
	{
		dvars::r_d3d9ex = game::Dvar_RegisterBool(
			/* name		*/ "r_d3d9ex",
			/* desc		*/ "extended d3d9 interface",
			/* default	*/ true,
			/* flags	*/ game::dvar_flags::saved);

		// hook Interface creation
		utils::hook::set(0x69142C, d3d9ex::direct3d_create9_stub);
	}
}

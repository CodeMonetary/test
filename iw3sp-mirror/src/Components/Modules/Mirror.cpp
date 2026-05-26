// Mirror.cpp -- port of iw3xo v38.2 mirror functionality to iw3sp.exe.
//
// Stage A scope:
//   r_fullMirror 0/1/2          -- backbuffer horizontal flip.
//   r_fullMirrorDepth 0/1       -- companion main-INTZ-DSV horizontal flip
//                                  for ReShade MXAO compatibility.
//   r_mirrorViewmodel_rtt 0/1   -- off-screen RT capture of gun pass +
//                                  UV-flipped composite onto tonemap source
//                                  or backbuffer.
//   r_mirrorViewmodel_rttTonemapInject 0/1
//   r_mirrorViewmodel_rttEarlyComposite 0/1
//   r_mirrorViewmodel_rttBlend 0/1/2/3
//   r_mirrorViewmodel_compositeSrgb 0/1/2/3
//   r_mirrorViewmodel_depthFix 0/1/2/3
//   r_mirrorViewmodel_clearRttDepth 0/1
//   r_mirrorViewmodel_flipVSCF 0/1/2     (matrix-flip fallback when rtt=0)
//   r_mirrorViewmodel_flipFollow N
//   r_mirrorViewmodel_flipAxis 0..9
//   r_mirrorViewmodel_flipReg N
//
// NOT in scope (Stages B/C):
//   r_hudMirror, r_mirrorViewmodel_mirrorFx, mirrorViewmodel_mirrorFxAxis,
//   FX_SpawnOriented hook, CG_DObjGetWorldBoneMatrix hook,
//   R_SetViewParmsForScene hook.
//
// Behaviour is structurally identical to the v38.2 MP path - same render
// states, same UV layouts, same fingerprints. Only differences from MP:
//   * Dvar pointers live under top-level Dvars:: (vs MP dvars::).
//   * Engine printf uses Game::Com_PrintMessage (vs game::).
//   * Diagnostic logging is fronted by mirror_log_level() instead of MP's
//     hudlog_level(), so MP's dump file mechanism is not pulled in.
//
// The mirror_rtt:: helpers and hud_start_detect:: detector are placed in an
// anonymous namespace inside Components::Mirror so they don't collide with
// any future iw3sp-mod-rtx names.

#include "STDInc.hpp"
#include "Mirror.hpp"

#include <d3dx9shader.h>

namespace Mirror
{
namespace
{
	// ---------- engine-print thin wrapper ----------
	static void engine_print(const char* msg)
	{
		if (Game::Com_PrintMessage)
			Game::Com_PrintMessage(0, msg, 0);
	}

	static int mirror_log_level()
	{
		if (Dvars::r_mirrorViewmodel_log)
			return Dvars::r_mirrorViewmodel_log->current.integer;
		return 0;
	}

	static uint32_t s_frame_counter = 0;

	// ----------------------------------------------------------------------
	// mirror_rtt :: render-to-texture gun mirror + fullscreen flip resources.
	// Verbatim port of MP v38.2 d3d9ex.cpp lines 42-951.
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
		static bool g_pass_active                = false;
		static bool g_in_segment                 = false;
		static bool g_pending_early_composite    = false;
		static bool g_final_composite_done_this_frame = false;
		static bool g_dhp_seen_this_frame        = false;
		static bool g_dhp_seen_prev_frame        = false;
		static int  g_dhp_count_this_frame       = 0;
		static int  g_begin_seg_count_this_frame = 0;
		static int  g_end_seg_count_this_frame   = 0;
		static int  g_inject_calls_this_frame    = 0;
		static int  g_inject_ok_count_this_frame = 0;

		// fullMirror statics
		static IDirect3DTexture9* g_flip_tex     = nullptr;
		static IDirect3DSurface9* g_flip_surf    = nullptr;
		static int  g_flip_w                     = 0;
		static int  g_flip_h                     = 0;
		static bool g_pending_fullmirror_flip          = false;
		static bool g_pending_fullmirror_flip_hud_gated = false;

		// v37.2 tonemap PS cache (weak ref; dropped on Reset).
		static IDirect3DPixelShader9* g_tonemap_ps_cache = nullptr;

		static inline bool match_tonemap_structural(float c70, float c71, float c72, float c73)
		{
			auto fapprox_eq = [](float a, float b) { float d = a - b; if (d < 0) d = -d; return d < 1e-4f; };
			return c70 < 0.0f && c70 > -1.5f
				&& fapprox_eq(c70, c71) && fapprox_eq(c70, c72)
				&& c73 > 0.5f && c73 < 20.0f;
		}

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
				if (!g_tonemap_ps_cache && cur) g_tonemap_ps_cache = cur;
			}
			else if (same_rgb && cur && cur == g_tonemap_ps_cache)
			{
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
			++g_begin_seg_count_this_frame;
		}

		static void end_segment(IDirect3DDevice9* dev)
		{
			if (!g_in_segment) return;
			g_in_segment = false;
			if (g_saved_color) { dev->SetRenderTarget(0, g_saved_color); g_saved_color->Release(); g_saved_color = nullptr; }
			if (g_saved_depth) { dev->SetDepthStencilSurface(g_saved_depth); g_saved_depth->Release(); g_saved_depth = nullptr; }
			else                 dev->SetDepthStencilSurface(nullptr);
			++g_end_seg_count_this_frame;
		}

		// ReShade MXAO main-depth fix (3 modes; see Mirror.hpp / dvar help)
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
			++g_inject_calls_this_frame;
			if (g_in_segment) end_segment(dev);
			if (!g_pass_active) return false;

			IDirect3DBaseTexture9* source_base = nullptr;
			IDirect3DTexture9* source_tex = nullptr;
			IDirect3DSurface9* source_surface = nullptr;
			IDirect3DSurface9* prev_color = nullptr;
			IDirect3DSurface9* prev_depth = nullptr;
			IDirect3DStateBlock9* sb = nullptr;
			bool ok = false;
			const int blend_mode = Dvars::r_mirrorViewmodel_rttBlend
				? Dvars::r_mirrorViewmodel_rttBlend->current.integer : 2;
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
				g_final_composite_done_this_frame = true;
				++g_inject_ok_count_this_frame;
			}
			return ok;
		}

		static void final_composite(IDirect3DDevice9* dev)
		{
			if (g_in_segment) end_segment(dev);
			if (!g_pass_active) return;
			g_pass_active = false;
			g_final_composite_done_this_frame = true;

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
			const int srgb_mode = Dvars::r_mirrorViewmodel_compositeSrgb
				? Dvars::r_mirrorViewmodel_compositeSrgb->current.integer : 1;
			const BOOL srgb_read  = (srgb_mode == 2 || srgb_mode == 3) ? TRUE : FALSE;
			const BOOL srgb_write = (srgb_mode == 1 || srgb_mode == 3) ? TRUE : FALSE;
			dev->SetRenderState(D3DRS_SRGBWRITEENABLE,  srgb_write);
			dev->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
			dev->SetRenderState(D3DRS_COLORWRITEENABLE,
				D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
				D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
			dev->SetRenderState(D3DRS_STENCILENABLE,    FALSE);

			const int blend_mode = Dvars::r_mirrorViewmodel_rttBlend
				? Dvars::r_mirrorViewmodel_rttBlend->current.integer : 2;
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
			if (bb_surface) { bb_surface->Release(); bb_surface = nullptr; }
			if (prev_color) { prev_color->Release(); prev_color = nullptr; }
			if (sb) { sb->Apply(); sb->Release(); }
		}

		// ---- fullscreen flip ----
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

		// ---- main-DSV INTZ depth flip (ReShade MXAO companion) ----
		static const D3DFORMAT FOURCC_INTZ = (D3DFORMAT)MAKEFOURCC('I','N','T','Z');
		static IDirect3DTexture9*     g_depth_flip_intz = nullptr;
		static IDirect3DSurface9*     g_depth_flip_surf = nullptr;
		static IDirect3DPixelShader9* g_depth_copy_ps   = nullptr;
		static int  g_depth_flip_w   = 0;
		static int  g_depth_flip_h   = 0;
		static bool g_depth_flip_ps_compile_failed = false;

		static void release_depth_flip_resources()
		{
			if (g_depth_flip_surf) { g_depth_flip_surf->Release(); g_depth_flip_surf = nullptr; }
			if (g_depth_flip_intz) { g_depth_flip_intz->Release(); g_depth_flip_intz = nullptr; }
			if (g_depth_copy_ps)   { g_depth_copy_ps->Release();   g_depth_copy_ps   = nullptr; }
			g_depth_flip_w = g_depth_flip_h = 0;
			g_depth_flip_ps_compile_failed = false;
		}

		static bool ensure_depth_flip_intz(IDirect3DDevice9* dev, UINT w, UINT h)
		{
			if (g_depth_flip_intz && (int)w == g_depth_flip_w && (int)h == g_depth_flip_h) return true;
			if (g_depth_flip_surf) { g_depth_flip_surf->Release(); g_depth_flip_surf = nullptr; }
			if (g_depth_flip_intz) { g_depth_flip_intz->Release(); g_depth_flip_intz = nullptr; }
			g_depth_flip_w = g_depth_flip_h = 0;
			if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL,
				FOURCC_INTZ, D3DPOOL_DEFAULT, &g_depth_flip_intz, nullptr))) return false;
			if (FAILED(g_depth_flip_intz->GetSurfaceLevel(0, &g_depth_flip_surf)))
			{
				g_depth_flip_intz->Release(); g_depth_flip_intz = nullptr;
				return false;
			}
			g_depth_flip_w = (int)w;
			g_depth_flip_h = (int)h;
			return true;
		}

		static bool ensure_depth_copy_ps(IDirect3DDevice9* dev)
		{
			if (g_depth_copy_ps) return true;
			if (g_depth_flip_ps_compile_failed) return false;

			static const char* asm_src =
				"ps_2_0\n"
				"dcl_2d s0\n"
				"dcl t0.xy\n"
				"texld r0, t0, s0\n"
				"mov oC0, r0\n"
				"mov oDepth, r0.x\n";

			ID3DXBuffer* code   = nullptr;
			ID3DXBuffer* errors = nullptr;
			HRESULT hr = D3DXAssembleShader(asm_src, (UINT)strlen(asm_src), nullptr, nullptr, 0, &code, &errors);
			if (errors) errors->Release();
			if (FAILED(hr) || !code)
			{
				g_depth_flip_ps_compile_failed = true;
				return false;
			}
			hr = dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &g_depth_copy_ps);
			code->Release();
			if (FAILED(hr) || !g_depth_copy_ps)
			{
				g_depth_flip_ps_compile_failed = true;
				return false;
			}
			return true;
		}

		static bool get_main_dsv_intz_texture(IDirect3DSurface9* dsv, IDirect3DTexture9** out_tex)
		{
			if (!dsv || !out_tex) return false;
			*out_tex = nullptr;
			IDirect3DTexture9* tex = nullptr;
			if (FAILED(dsv->GetContainer(__uuidof(IDirect3DTexture9), (void**)&tex)) || !tex) return false;
			D3DSURFACE_DESC sd;
			if (FAILED(tex->GetLevelDesc(0, &sd)) || sd.Format != FOURCC_INTZ)
			{
				tex->Release();
				return false;
			}
			*out_tex = tex;
			return true;
		}

		static bool do_main_depth_flip(IDirect3DDevice9* dev)
		{
			IDirect3DSurface9* main_dsv = nullptr;
			if (FAILED(dev->GetDepthStencilSurface(&main_dsv)) || !main_dsv) return false;

			IDirect3DTexture9* main_dsv_tex = nullptr;
			if (!get_main_dsv_intz_texture(main_dsv, &main_dsv_tex))
			{
				main_dsv->Release();
				return false;
			}

			D3DSURFACE_DESC dsv_desc;
			main_dsv->GetDesc(&dsv_desc);

			if (!ensure_depth_flip_intz(dev, dsv_desc.Width, dsv_desc.Height) ||
				!ensure_depth_copy_ps(dev))
			{
				main_dsv_tex->Release();
				main_dsv->Release();
				return false;
			}

			IDirect3DSurface9*    prev_color = nullptr;
			IDirect3DStateBlock9* sb         = nullptr;
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;
			if (FAILED(dev->GetRenderTarget(0, &prev_color))) prev_color = nullptr;

			auto set_common_state = [&]()
			{
				dev->SetVertexShader(nullptr);
				dev->SetPixelShader(g_depth_copy_ps);
				dev->SetVertexDeclaration(nullptr);
				dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
				dev->SetRenderState(D3DRS_ZENABLE,           TRUE);
				dev->SetRenderState(D3DRS_ZWRITEENABLE,      TRUE);
				dev->SetRenderState(D3DRS_ZFUNC,             D3DCMP_ALWAYS);
				dev->SetRenderState(D3DRS_CULLMODE,          D3DCULL_NONE);
				dev->SetRenderState(D3DRS_LIGHTING,          FALSE);
				dev->SetRenderState(D3DRS_FOGENABLE,         FALSE);
				dev->SetRenderState(D3DRS_ALPHABLENDENABLE,  FALSE);
				dev->SetRenderState(D3DRS_ALPHATESTENABLE,   FALSE);
				dev->SetRenderState(D3DRS_STENCILENABLE,     FALSE);
				dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
				dev->SetRenderState(D3DRS_SRGBWRITEENABLE,   FALSE);
				dev->SetRenderState(D3DRS_COLORWRITEENABLE,  0);
				dev->SetSamplerState(0, D3DSAMP_MINFILTER,   D3DTEXF_POINT);
				dev->SetSamplerState(0, D3DSAMP_MAGFILTER,   D3DTEXF_POINT);
				dev->SetSamplerState(0, D3DSAMP_ADDRESSU,    D3DTADDRESS_CLAMP);
				dev->SetSamplerState(0, D3DSAMP_ADDRESSV,    D3DTADDRESS_CLAMP);
				dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
			};

			const float W = (float)dsv_desc.Width;
			const float H = (float)dsv_desc.Height;
			struct V { float x, y, z, rhw, u, v; };

			dev->SetDepthStencilSurface(g_depth_flip_surf);
			set_common_state();
			dev->SetTexture(0, main_dsv_tex);
			{
				V quad[4] = {
					{ -0.5f,    -0.5f,    0.0f, 1.0f, 1.0f, 0.0f },
					{  W-0.5f,  -0.5f,    0.0f, 1.0f, 0.0f, 0.0f },
					{ -0.5f,     H-0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
					{  W-0.5f,   H-0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
				};
				dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
			}

			dev->SetDepthStencilSurface(main_dsv);
			set_common_state();
			dev->SetTexture(0, g_depth_flip_intz);
			{
				V quad[4] = {
					{ -0.5f,    -0.5f,    0.0f, 1.0f, 0.0f, 0.0f },
					{  W-0.5f,  -0.5f,    0.0f, 1.0f, 1.0f, 0.0f },
					{ -0.5f,     H-0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
					{  W-0.5f,   H-0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
				};
				dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
			}

			dev->SetTexture(0, nullptr);

			if (prev_color) { dev->SetRenderTarget(0, prev_color); }
			if (sb) { sb->Apply(); sb->Release(); }
			if (prev_color) { prev_color->Release(); }
			main_dsv_tex->Release();
			main_dsv->Release();
			return true;
		}

		static inline void do_main_depth_flip_if_enabled(IDirect3DDevice9* dev)
		{
			if (!Dvars::r_fullMirrorDepth) return;
			if (Dvars::r_fullMirrorDepth->current.integer == 0) return;
			do_main_depth_flip(dev);
		}

		static void on_device_reset()
		{
			if (g_saved_color) { g_saved_color->Release(); g_saved_color = nullptr; }
			if (g_saved_depth) { g_saved_depth->Release(); g_saved_depth = nullptr; }
			g_pass_active            = false;
			g_in_segment             = false;
			g_pending_early_composite = false;
			g_pending_fullmirror_flip = false;
			g_pending_fullmirror_flip_hud_gated = false;
			g_tonemap_ps_cache       = nullptr;
			release_targets();
			release_flip_target();
			release_depth_flip_resources();
		}
	} // namespace mirror_rtt

	// ----------------------------------------------------------------------
	// hud_start_detect :: 4-state HUD-start signature observer.
	// Verbatim port of MP v38.2 d3d9ex.cpp lines 973-1004 plus extended
	// SP-specific render-state shadows for diagnostic dumps.
	// ----------------------------------------------------------------------
	namespace hud_start_detect
	{
		static DWORD g_shadow_cull = 3;
		static DWORD g_shadow_abe  = 0;
		static DWORD g_shadow_sb   = D3DBLEND_ONE;
		static DWORD g_shadow_ate  = 0;
		// extra shadows for r_mirrorViewmodel_log >= 2 dump
		static DWORD g_shadow_db   = D3DBLEND_ZERO;
		static DWORD g_shadow_sba  = D3DBLEND_ONE;
		static DWORD g_shadow_dba  = D3DBLEND_ZERO;
		static DWORD g_shadow_ze   = 1;
		static DWORD g_shadow_zw   = 1;
		static DWORD g_shadow_fill = D3DFILL_SOLID;

		// Set by the PSCF c7 tonemap fingerprint to gate the diagnostic
		// window.  Reset on BeginScene so we only dump the post-tonemap-
		// to-HUD-start interval per frame.
		static bool g_diag_window_open = false;
		static int  g_diag_ate_rise_seen = 0;
		static int  g_diag_pscf7_fires_this_frame = 0;

		static bool observe(D3DRENDERSTATETYPE State, DWORD Value)
		{
			bool fired = false;
			switch (State)
			{
			case D3DRS_CULLMODE:         g_shadow_cull = Value; break;
			case D3DRS_ALPHABLENDENABLE: g_shadow_abe  = Value; break;
			case D3DRS_SRCBLEND:         g_shadow_sb   = Value; break;
			case D3DRS_DESTBLEND:        g_shadow_db   = Value; break;
			case D3DRS_SRCBLENDALPHA:    g_shadow_sba  = Value; break;
			case D3DRS_DESTBLENDALPHA:   g_shadow_dba  = Value; break;
			case D3DRS_ZENABLE:          g_shadow_ze   = Value; break;
			case D3DRS_ZWRITEENABLE:     g_shadow_zw   = Value; break;
			case D3DRS_FILLMODE:         g_shadow_fill = Value; break;
			case D3DRS_ALPHATESTENABLE:
				if (Value != 0 && g_shadow_ate == 0)
				{
					fired = (g_shadow_cull == 1
						&& g_shadow_abe != 0
						&& g_shadow_sb == 2);

					// Diagnostic dump: print full RS snapshot on every ATE
					// rising edge while the post-tonemap window is open and
					// the user enabled log >= 2.  Limited to first 8 per frame
					// so we don't flood console.
					if (g_diag_window_open
						&& g_diag_ate_rise_seen < 32
						&& mirror_log_level() >= 2)
					{
						++g_diag_ate_rise_seen;
						char buf[256];
						_snprintf_s(buf, sizeof(buf),
							"[mirror:hud-diag] ate-rise #%d  cull=%lu abe=%lu sb=%lu db=%lu sba=%lu dba=%lu ze=%lu zw=%lu fill=%lu  matched=%d\n",
							g_diag_ate_rise_seen,
							(unsigned long)g_shadow_cull,
							(unsigned long)g_shadow_abe,
							(unsigned long)g_shadow_sb,
							(unsigned long)g_shadow_db,
							(unsigned long)g_shadow_sba,
							(unsigned long)g_shadow_dba,
							(unsigned long)g_shadow_ze,
							(unsigned long)g_shadow_zw,
							(unsigned long)g_shadow_fill,
							(int)fired);
						engine_print(buf);
					}
				}
				g_shadow_ate = Value;
				break;
			default: break;
			}
			return fired;
		}

		static void open_diag_window()
		{
			// First open of the frame resets the rise counter.  Subsequent
			// PSCF c7 fires within the same frame just track the call count
			// and leave the rise counter intact so we see *all* ATE rises
			// in the post-tonemap-to-next-BeginScene window.
			if (!g_diag_window_open)
			{
				g_diag_window_open   = true;
				g_diag_ate_rise_seen = 0;
			}
			++g_diag_pscf7_fires_this_frame;
		}

		static void close_diag_window()
		{
			g_diag_window_open = false;
		}

		static void reset_frame_counters()
		{
			g_diag_pscf7_fires_this_frame = 0;
		}

		static void dump_state_snapshot(const char* tag)
		{
			if (mirror_log_level() < 2) return;
			char buf[256];
			_snprintf_s(buf, sizeof(buf),
				"[mirror:%s] cull=%lu abe=%lu sb=%lu db=%lu sba=%lu dba=%lu ate=%lu ze=%lu zw=%lu fill=%lu\n",
				tag,
				(unsigned long)g_shadow_cull,
				(unsigned long)g_shadow_abe,
				(unsigned long)g_shadow_sb,
				(unsigned long)g_shadow_db,
				(unsigned long)g_shadow_sba,
				(unsigned long)g_shadow_dba,
				(unsigned long)g_shadow_ate,
				(unsigned long)g_shadow_ze,
				(unsigned long)g_shadow_zw,
				(unsigned long)g_shadow_fill);
			engine_print(buf);
		}

		static int pscf7_fires_this_frame()
		{
			return g_diag_pscf7_fires_this_frame;
		}
	} // namespace hud_start_detect

	// ----------------------------------------------------------------------
	// Per-instance follow-window counter.
	// ----------------------------------------------------------------------
	static int s_mirror_vscf_follow_remaining = 0;
	static float s_flip_scratch[16];
} // anonymous namespace


// ===========================================================================
// Public API
// ===========================================================================

void OnBeginScene(IDirect3DDevice9* /*dev*/)
{
	static bool s_announced = false;
	if (!s_announced)
	{
		s_announced = true;
		engine_print("[mirror] D3D9Ex wrapper active, mirror hooks installed (v38.2 SP port).\n");
	}

	mirror_rtt::g_final_composite_done_this_frame = false;
	mirror_rtt::g_dhp_seen_prev_frame = mirror_rtt::g_dhp_seen_this_frame;
	mirror_rtt::g_dhp_seen_this_frame = false;
	mirror_rtt::g_dhp_count_this_frame       = 0;
	mirror_rtt::g_begin_seg_count_this_frame = 0;
	mirror_rtt::g_end_seg_count_this_frame   = 0;
	mirror_rtt::g_inject_calls_this_frame    = 0;
	mirror_rtt::g_inject_ok_count_this_frame = 0;
	hud_start_detect::close_diag_window();
	hud_start_detect::reset_frame_counters();
	++s_frame_counter;
}

void OnEndScene(IDirect3DDevice9* dev)
{
	// 1) safety-net composite for unresolved gun pass
	if (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment)
	{
		mirror_rtt::final_composite(dev);
	}

	// 2) r_fullMirror == 2 -> flip BB+HUD at end of scene
	{
		const int full_mirror_eos = Dvars::r_fullMirror
			? Dvars::r_fullMirror->current.integer : 0;
		if (full_mirror_eos == 2)
		{
			if (mirror_rtt::do_fullscreen_flip(dev))
			{
				mirror_rtt::do_main_depth_flip_if_enabled(dev);
			}
		}
	}

	// 3) hud-gated fallback (menu/console frame; no HUD draws this frame)
	if (mirror_rtt::g_pending_fullmirror_flip_hud_gated)
	{
		// Diagnostic: dump RS state right before the fallback fires.
		// This is the state at EndScene -- if HUD drew, this captures
		// the post-HUD render-state that the engine leaves behind.
		hud_start_detect::dump_state_snapshot("hud-fallback");
		mirror_rtt::g_pending_fullmirror_flip_hud_gated = false;
		if (mirror_rtt::do_fullscreen_flip(dev))
		{
			mirror_rtt::do_main_depth_flip_if_enabled(dev);
		}
	}

	// 4) MXAO ReShade phantom-AO mitigation (clear RTT depth)
	if (mirror_rtt::g_depth
		&& Dvars::r_mirrorViewmodel_clearRttDepth
		&& Dvars::r_mirrorViewmodel_clearRttDepth->current.integer != 0)
	{
		IDirect3DSurface9* prev_color = nullptr;
		IDirect3DSurface9* prev_depth = nullptr;
		dev->GetRenderTarget(0, &prev_color);
		dev->GetDepthStencilSurface(&prev_depth);
		if (mirror_rtt::g_color) dev->SetRenderTarget(0, mirror_rtt::g_color);
		dev->SetDepthStencilSurface(mirror_rtt::g_depth);
		dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
		if (prev_color) { dev->SetRenderTarget(0, prev_color); prev_color->Release(); }
		if (prev_depth) { dev->SetDepthStencilSurface(prev_depth); prev_depth->Release(); }
		else            { dev->SetDepthStencilSurface(nullptr); }
	}

	// 5) per-frame log
	if (mirror_log_level() >= 1 && (s_frame_counter % 60u) == 0u)
	{
		char buf[320];
		_snprintf_s(buf, sizeof(buf),
			"[mirror] f=%u fm=%d rtt=%d dhp=%d/%d bsg=%d esg=%d inj=%d/%d pass_end=%d pscf7=%d\n",
			s_frame_counter,
			Dvars::r_fullMirror ? Dvars::r_fullMirror->current.integer : 0,
			Dvars::r_mirrorViewmodel_rtt ? Dvars::r_mirrorViewmodel_rtt->current.integer : 0,
			(int)mirror_rtt::g_dhp_seen_this_frame,
			mirror_rtt::g_dhp_count_this_frame,
			mirror_rtt::g_begin_seg_count_this_frame,
			mirror_rtt::g_end_seg_count_this_frame,
			mirror_rtt::g_inject_ok_count_this_frame,
			mirror_rtt::g_inject_calls_this_frame,
			(int)mirror_rtt::g_pass_active,
			hud_start_detect::pscf7_fires_this_frame());
		engine_print(buf);

		// Also dump the final state snapshot at EndScene every 60th frame
		// so we can correlate it with the ate-rise stream above.
		hud_start_detect::dump_state_snapshot("eos-state");
	}
}

void OnReset()
{
	mirror_rtt::on_device_reset();
}

void OnPresent()
{
	s_mirror_vscf_follow_remaining = 0;
}

void OnSetVertexShaderConstantF(IDirect3DDevice9* dev,
	UINT StartRegister, const float* pConstantData, UINT Vector4fCount,
	const float** outConstantData)
{
	*outConstantData = pConstantData;
	if (!pConstantData) return;

	const bool is_mtx_at_zero = (StartRegister == 0 && Vector4fCount == 4);
	float c23 = 0.0f;
	if (is_mtx_at_zero) c23 = pConstantData[11];
	const bool is_depth_hack_proj = is_mtx_at_zero && (c23 < -0.02f && c23 > -0.50f);
	const bool is_std_proj        = is_mtx_at_zero && (c23 < -1.00f);

	const int flipVSCF = Dvars::r_mirrorViewmodel_flipVSCF
		? Dvars::r_mirrorViewmodel_flipVSCF->current.integer : 0;
	const int flipFollow = Dvars::r_mirrorViewmodel_flipFollow
		? Dvars::r_mirrorViewmodel_flipFollow->current.integer : 0;
	const int flipAxis = Dvars::r_mirrorViewmodel_flipAxis
		? Dvars::r_mirrorViewmodel_flipAxis->current.integer : 4;
	const int flipReg = Dvars::r_mirrorViewmodel_flipReg
		? Dvars::r_mirrorViewmodel_flipReg->current.integer : 0;

	if (is_depth_hack_proj)
	{
		s_mirror_vscf_follow_remaining = flipFollow;
		mirror_rtt::g_dhp_seen_this_frame = true;
		++mirror_rtt::g_dhp_count_this_frame;
	}
	else if (is_std_proj)
	{
		s_mirror_vscf_follow_remaining = 0;
	}

	const int rtt_on = Dvars::r_mirrorViewmodel_rtt
		? Dvars::r_mirrorViewmodel_rtt->current.integer : 0;
	if (rtt_on)
	{
		if (is_depth_hack_proj)
		{
			mirror_rtt::begin_segment(dev);
		}
		else if (is_mtx_at_zero && mirror_rtt::g_in_segment)
		{
			mirror_rtt::end_segment(dev);
		}
	}

	const bool is_target_mtx = (Vector4fCount == 4 && (int)StartRegister == flipReg);
	bool apply_flip = false;
	if (is_target_mtx && flipVSCF != 0 && !rtt_on)
	{
		if (flipReg == 0)
		{
			if (flipVSCF == 1 && is_depth_hack_proj) apply_flip = true;
			if (flipVSCF == 2 && (is_depth_hack_proj || s_mirror_vscf_follow_remaining > 0))
				apply_flip = true;
		}
		else
		{
			if (s_mirror_vscf_follow_remaining > 0) apply_flip = true;
		}
	}

	if (apply_flip)
	{
		for (int i = 0; i < 16; ++i) s_flip_scratch[i] = pConstantData[i];
		switch (flipAxis) {
		case 0: case 1: case 2: case 3: {
			const int base = flipAxis * 4;
			s_flip_scratch[base+0] = -s_flip_scratch[base+0];
			s_flip_scratch[base+1] = -s_flip_scratch[base+1];
			s_flip_scratch[base+2] = -s_flip_scratch[base+2];
			s_flip_scratch[base+3] = -s_flip_scratch[base+3];
		} break;
		case 4: case 5: case 6: case 7: {
			const int off = flipAxis - 4;
			s_flip_scratch[off+0]  = -s_flip_scratch[off+0];
			s_flip_scratch[off+4]  = -s_flip_scratch[off+4];
			s_flip_scratch[off+8]  = -s_flip_scratch[off+8];
			s_flip_scratch[off+12] = -s_flip_scratch[off+12];
		} break;
		case 8:
			for (int i = 0; i < 16; ++i) s_flip_scratch[i] = -s_flip_scratch[i];
			break;
		case 9:
			s_flip_scratch[4]  = -s_flip_scratch[4];
			s_flip_scratch[8]  = -s_flip_scratch[8];
			s_flip_scratch[12] = -s_flip_scratch[12];
			break;
		}
		*outConstantData = s_flip_scratch;
	}

	if (is_mtx_at_zero && !is_depth_hack_proj && s_mirror_vscf_follow_remaining > 0)
	{
		--s_mirror_vscf_follow_remaining;
	}
}

void OnSetPixelShaderConstantF(IDirect3DDevice9* dev,
	UINT StartRegister, const float* pConstantData, UINT Vector4fCount)
{
	if (!pConstantData) return;

	const int rtt_on_pscf = Dvars::r_mirrorViewmodel_rtt
		? Dvars::r_mirrorViewmodel_rtt->current.integer : 0;
	const int rtt_early = Dvars::r_mirrorViewmodel_rttEarlyComposite
		? Dvars::r_mirrorViewmodel_rttEarlyComposite->current.integer : 1;

	if (rtt_on_pscf && rtt_early != 0
		&& StartRegister == 7 && Vector4fCount >= 1
		&& (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment))
	{
		const float c70 = pConstantData[0];
		const float c71 = pConstantData[1];
		const float c72 = pConstantData[2];
		const float c73 = pConstantData[3];
		const bool is_pre_hud_signal = mirror_rtt::match_tonemap_signal(dev, c70, c71, c72, c73);
		if (is_pre_hud_signal)
		{
			const int dfix_mode = Dvars::r_mirrorViewmodel_depthFix
				? Dvars::r_mirrorViewmodel_depthFix->current.integer : 0;
			if (dfix_mode > 0)
			{
				if (mirror_rtt::g_in_segment) mirror_rtt::end_segment(dev);
				mirror_rtt::apply_main_depth_fix(dev, dfix_mode);
			}

			const int tonemap_inject = Dvars::r_mirrorViewmodel_rttTonemapInject
				? Dvars::r_mirrorViewmodel_rttTonemapInject->current.integer : 1;
			if (tonemap_inject && mirror_rtt::inject_into_tonemap_source(dev))
			{
				// gun merged into tonemap source -> final HUD draws over composited gun
			}
			else
			{
				if (mirror_rtt::g_in_segment) mirror_rtt::end_segment(dev);
				mirror_rtt::g_pending_early_composite = true;
			}
		}
	}

	// r_fullMirror == 1 trigger
	{
		const int full_mirror = Dvars::r_fullMirror
			? Dvars::r_fullMirror->current.integer : 0;
		if (full_mirror == 1 && StartRegister == 7 && Vector4fCount >= 1)
		{
			const float c70 = pConstantData[0];
			const float c71 = pConstantData[1];
			const float c72 = pConstantData[2];
			const float c73 = pConstantData[3];
			const bool is_pre_hud_signal = mirror_rtt::match_tonemap_signal(dev, c70, c71, c72, c73);
			if (is_pre_hud_signal)
			{
				// Open the diagnostic window so hud_start_detect::observe
				// dumps RS snapshots for the post-tonemap interval when
				// r_mirrorViewmodel_log >= 2.  Used to reverse-engineer the
				// SP HUD-start signature (MP v38.2's 4-state sig fails to
				// fire in SP, so we cannot gate on it yet).
				hud_start_detect::open_diag_window();

				// Gating mode selector:
				//   r_fullMirror_gateMode 0 (default) = v38.2 MP behaviour:
				//      post-DRAW when r_blur == 0, HUD-gated when r_blur > 0.
				//      Works for normal frames but the SP damage-overlay
				//      composite (not r_blur-driven) ghosts over the gun.
				//   r_fullMirror_gateMode 1 = always HUD-gated.  Damage
				//      stable but HUD also gets mirrored (EndScene fallback
				//      fires every frame because the SP HUD-start signature
				//      does not match v38.2's 4-state pattern yet).
				//   r_fullMirror_gateMode 2 = always post-DRAW (v37 path).
				//      Damage breaks but no HUD-flip ever happens.
				const int gate_mode = Dvars::r_fullMirror_gateMode
					? Dvars::r_fullMirror_gateMode->current.integer : 0;

				bool use_hud_gated = false;
				if (gate_mode == 1)
				{
					use_hud_gated = true;
				}
				else if (gate_mode == 2)
				{
					use_hud_gated = false;
				}
				else
				{
					Game::dvar_s* d_blur = Dvars::Functions::Dvar_FindVar
						? Dvars::Functions::Dvar_FindVar("r_blur") : nullptr;
					use_hud_gated = d_blur && d_blur->current.value > 0.0f;
				}

				if (use_hud_gated)
					mirror_rtt::g_pending_fullmirror_flip_hud_gated = true;
				else
					mirror_rtt::g_pending_fullmirror_flip = true;
			}
		}
	}
}

static void flush_post_draw(IDirect3DDevice9* dev)
{
	if (mirror_rtt::g_pending_early_composite)
	{
		mirror_rtt::g_pending_early_composite = false;
		if (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment)
		{
			if (mirror_rtt::g_in_segment) mirror_rtt::end_segment(dev);
			mirror_rtt::final_composite(dev);
		}
	}
	if (mirror_rtt::g_pending_fullmirror_flip)
	{
		mirror_rtt::g_pending_fullmirror_flip = false;
		if (mirror_rtt::do_fullscreen_flip(dev))
		{
			mirror_rtt::do_main_depth_flip_if_enabled(dev);
		}
	}
}

void OnDrawPrimitive_Post(IDirect3DDevice9* dev)
{
	flush_post_draw(dev);
}

void OnDrawIndexedPrimitive_Post(IDirect3DDevice9* dev)
{
	flush_post_draw(dev);
}

void OnSetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE State, DWORD Value)
{
	const bool hud_start_fired = hud_start_detect::observe(State, Value);
	if (hud_start_fired && mirror_rtt::g_pending_fullmirror_flip_hud_gated)
	{
		mirror_rtt::g_pending_fullmirror_flip_hud_gated = false;
		if (mirror_rtt::do_fullscreen_flip(dev))
			mirror_rtt::do_main_depth_flip_if_enabled(dev);
	}
}

void RegisterDvars()
{
	// Registered in CommonPatch.cpp -- see Dvars.cpp / Dvars.hpp /
	// CommonPatch.cpp for the actual Dvar_RegisterX calls. Keeping this
	// stub allows code searches for the helper to land somewhere sane.
}

// ----------------------------------------------------------------------
// r_mirrorInput :: input-side mirroring (mouse-X + strafe-X).
//
//   Goal: pair the visual r_fullMirror flip with a mirrored control
//   scheme so input semantics match what the player sees.  Forward,
//   back, pitch (up/down) and jump are untouched -- only horizontal
//   axes are negated.
//
//   Hooks:
//     RawMouse::CL_MouseEvent    -> Mirror::OnMouseDelta(&dx, &dy)
//                                   (called before mouseDx accumulation
//                                   so engine sees the inverted value)
//     Gamepad::CL_MouseMove tail -> Mirror::OnUserCmd(cmd)
//                                   (post-processes the populated
//                                   usercmd_s right before consumption)
// ----------------------------------------------------------------------
void OnMouseDelta(int* dx, int* /*dy*/)
{
	if (!dx) return;
	if (!Dvars::r_mirrorInput) return;
	if (Dvars::r_mirrorInput->current.integer == 0) return;
	*dx = -(*dx);
}

void OnUserCmd(Game::usercmd_s* cmd)
{
	if (!cmd) return;
	if (!Dvars::r_mirrorInput) return;
	if (Dvars::r_mirrorInput->current.integer == 0) return;
	// rightmove is signed char (-127..127).  Negation is symmetric in
	// this range except for the exact value -128 which char cannot
	// hold; cast through int to be safe and clamp.
	int v = -(int)cmd->rightmove;
	if (v >  127) v =  127;
	if (v < -127) v = -127;
	cmd->rightmove = (char)v;
}

} // namespace Mirror

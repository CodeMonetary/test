// https://github.com/IW4x/iw4x-client/blob/develop/src/Components/Modules/D3D9Ex.cpp
#include "std_include.hpp"

namespace components
{
	// r_mirrorViewmodel dump counters (defined in _renderer.cpp)
	extern void mirror_dump_inc_rs();
	extern void mirror_dump_inc_vscf();
	extern void mirror_dump_inc_pscf();
	extern void mirror_dump_inc_draw();

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
		}

		static bool inject_into_tonemap_source(IDirect3DDevice9* dev)
		{
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
			if (ok) g_pass_active = false;
			return ok;
		}

		static void final_composite(IDirect3DDevice9* dev)
		{
			if (g_in_segment) end_segment(dev); // safety net (no stdp seen before EndScene)
			if (!g_pass_active) return;
			g_pass_active   = false;

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

		static void on_device_reset()
		{
			if (g_saved_color) { g_saved_color->Release(); g_saved_color = nullptr; }
			if (g_saved_depth) { g_saved_depth->Release(); g_saved_depth = nullptr; }
			g_pass_active            = false;
			g_in_segment             = false;
			g_pending_early_composite = false;
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
			mirror_rtt::final_composite(m_pIDirect3DDevice9);
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

		if (_renderer::mirror_dump_frames_remaining > 0)
		{
			_renderer::mirror_dump_write("\n=== end of frame %d (EndScene) ===\n",
				_renderer::mirror_dump_frame_counter);
			_renderer::mirror_dump_frame_counter++;
			_renderer::mirror_dump_frames_remaining--;
			if (_renderer::mirror_dump_frames_remaining == 0)
			{
				_renderer::mirror_dump_close();
			}
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
		if (_renderer::mirror_dump_active())
		{
			mirror_dump_inc_draw();
			_renderer::mirror_dump_write(
				"  DRAW raw prim=%u  follow=%d\n",
				PrimitiveCount, _renderer::mirror_vscf_follow_remaining);
		}
		HRESULT hr = m_pIDirect3DDevice9->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		if (mirror_rtt::g_pending_early_composite)
		{
			mirror_rtt::g_pending_early_composite = false;
			if (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment)
			{
				if (mirror_rtt::g_in_segment) mirror_rtt::end_segment(m_pIDirect3DDevice9);
				mirror_rtt::final_composite(m_pIDirect3DDevice9);
			}
		}
		return hr;
	}

	HRESULT d3d9ex::D3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
	{
		if (_renderer::mirror_dump_active())
		{
			mirror_dump_inc_draw();
			_renderer::mirror_dump_write(
				"  DRAW idx prim=%u nverts=%u  follow=%d\n",
				primCount, NumVertices, _renderer::mirror_vscf_follow_remaining);
		}
		HRESULT hr = m_pIDirect3DDevice9->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		// v22: composite the mirrored viewmodel right AFTER the engine's
		// final tonemap/output draw (the first draw following the PSCF c7
		// fingerprint). The pending flag was set by SetPixelShaderConstantF.
		if (mirror_rtt::g_pending_early_composite)
		{
			mirror_rtt::g_pending_early_composite = false;
			if (mirror_rtt::g_pass_active || mirror_rtt::g_in_segment)
			{
				if (mirror_rtt::g_in_segment) mirror_rtt::end_segment(m_pIDirect3DDevice9);
				mirror_rtt::final_composite(m_pIDirect3DDevice9);
			}
		}
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

		// Apply flip if: this upload is a 4-row matrix at the configured flipReg AND we are in
		// a gun pass (dhp itself, or within follow window when flipVSCF==2).
		// When rtt is on, the off-screen render path replaces matrix-flip; disable it.
		const bool is_target_mtx = (pConstantData && Vector4fCount == 4 && (int)StartRegister == flipReg);
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
			// v23: generalized tonemap-shader fingerprint. The exact values shift
			// based on the engine's gamma/exposure setting (live match observed
			// (-0.066, -0.066, -0.066, 2.773585), demo replay observed
			// (-0.013772, -0.013772, -0.013772, 1.222222)) but the STRUCTURE is
			// the same: the first three components are equal and slightly
			// negative, the fourth is the gamma exponent in [1.0, 5.0]. This
			// shape is unique to the final tonemap technique and never appears
			// in stencil-shadow / post-FX bloom / world / viewmodel passes.
			auto fapprox_eq = [](float a, float b) { float d = a - b; if (d < 0) d = -d; return d < 1e-4f; };
			const bool is_pre_hud_signal =
				c70 < 0.0f && c70 > -0.2f &&
				fapprox_eq(c70, c71) && fapprox_eq(c70, c72) &&
				c73 > 1.0f && c73 < 5.0f;
			if (is_pre_hud_signal)
			{
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

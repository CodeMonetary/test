#include "device_wrapper.hpp"
#include "d3d9_wrapper.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <windows.h>
#include <cmath>
#include <cstring>

namespace wf {

extern Config g_cfg;

// =====================================================================
//   Projection matrix detection
// =====================================================================
//
// CoD4 uploads its matrices to the vertex shader as 4 consecutive float4
// constants.  Several forms are possible:
//
//   - pure projection matrix (row-major, D3D convention):
//         m[0][0]  0        0       0
//         0        m[1][1]  0       0
//         0        0        m[2][2] 1
//         0        0        m[3][2] 0
//     (infinite-far variant: m[2][2]=1, m[3][2]=-zn)
//
//   - combined viewProjection (V*P): has non-zero m[i][3] columns, so
//     the "pure" signature is destroyed.
//
// We apply the X-mirror by multiplying the projection matrix on the right
// by diag(-1,1,1,1), which corresponds to negating column 0 of the matrix
// (i.e. m[i][0] for i in 0..3).  For a pure projection matrix only
// m[0][0] is non-zero in column 0, so the effect is m[0][0] *= -1.
//
// Combined WVP: flipping column 0 of WVP still corresponds to
// clip.x = -clip.x, which is the same visual effect.
//
// HOWEVER, because "does this matrix produce clip-space output?" is hard
// to know in general, we restrict ourselves to two detection regimes:
//
//   * Pure-projection signature: very distinctive, rarely mis-triggered.
//   * V*P signature derived from m[2][3] (close to 1 or -1) with sparse
//     m[i][3] elsewhere - still reasonably safe.
//
// Anything else is left alone.
// =====================================================================

static inline bool near_zero(float f, float eps = 1e-5f) {
    return std::fabs(f) < eps;
}
static inline bool approx(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) < eps;
}

// matrix laid out as 16 floats, row-major: row i = data[i*4 + j]
// The shader convention stores each row as one float4 constant, so data
// arriving from the engine via 4 consecutive VS constants is row-major.
static bool looks_like_pure_projection(const float* m) {
    // zeros in positions that must be zero for a pure projection
    const bool z01 = near_zero(m[0*4+1]);
    const bool z02 = near_zero(m[0*4+2]);
    const bool z03 = near_zero(m[0*4+3]);
    const bool z10 = near_zero(m[1*4+0]);
    const bool z12 = near_zero(m[1*4+2]);
    const bool z13 = near_zero(m[1*4+3]);
    const bool z20 = near_zero(m[2*4+0]);
    const bool z21 = near_zero(m[2*4+1]);
    const bool z30 = near_zero(m[3*4+0]);
    const bool z31 = near_zero(m[3*4+1]);

    // projection "tail"
    const bool p23_one   = approx(m[2*4+3], 1.0f, 1e-3f);
    const bool p33_zero  = near_zero(m[3*4+3], 1e-3f);

    const bool diag_nonzero =
        !near_zero(m[0*4+0]) && !near_zero(m[1*4+1]);

    return z01 && z02 && z03 && z10 && z12 && z13 &&
           z20 && z21 && z30 && z31 &&
           p23_one && p33_zero && diag_nonzero;
}

// Looser test: view*projection matrix. m[?][3] column has at most 3
// non-zero entries and m[3][3] is near zero (perspective).
static bool looks_like_view_projection(const float* m) {
    const bool p33_zero = near_zero(m[3*4+3], 1e-2f);
    // m[2][3] is typically non-negligible for a VP (comes from the
    // perspective term). Sum-of-squares of column 3 must be > 0.
    const float c3sq =
        m[0*4+3]*m[0*4+3] + m[1*4+3]*m[1*4+3] +
        m[2*4+3]*m[2*4+3] + m[3*4+3]*m[3*4+3];
    return p33_zero && c3sq > 0.1f;
}

static void flip_column0(float* m /*4x4 row-major*/) {
    m[0*4+0] = -m[0*4+0];
    m[1*4+0] = -m[1*4+0];
    m[2*4+0] = -m[2*4+0];
    m[3*4+0] = -m[3*4+0];
}

// =====================================================================
//   Construction / IUnknown
// =====================================================================

DeviceWrap::DeviceWrap(IDirect3DDevice9* real, D3D9Wrap* parent)
    : m_real(real), m_parent(parent) {
    m_runtime_enabled = g_cfg.enabled;
    logf("DeviceWrap: ctor this=%p real=%p parent=%p, flip enabled=%d, toggle vk=0x%02x",
         (void*)this, (void*)real, (void*)parent,
         (int)m_runtime_enabled, g_cfg.toggle_vk);
}

// {B18B10CE-2649-405A-870F-95F777D4313A} - IID_IDirect3DDevice9Ex
static const IID IID_IDirect3DDevice9Ex_local =
    { 0xB18B10CE, 0x2649, 0x405A, { 0x87, 0x0F, 0x95, 0xF7, 0x77, 0xD4, 0x31, 0x3A } };

HRESULT __stdcall DeviceWrap::QueryInterface(REFIID riid, void** ppv) {
    if (m_diag_qi < 16) {
        log_guid("DeviceWrap::QI", riid);
        ++m_diag_qi;
    }
    // Refuse IDirect3DDevice9Ex: the raw Ex pointer is different from our
    // IDirect3DDevice9 wrapper, so if we let it through the caller renders
    // through the raw device and bypasses our world-flip entirely.
    if (riid == IID_IDirect3DDevice9Ex_local) {
        if (ppv) *ppv = nullptr;
        logf("DeviceWrap::QI declined IDirect3DDevice9Ex");
        return E_NOINTERFACE;
    }
    HRESULT hr = m_real->QueryInterface(riid, ppv);
    if (SUCCEEDED(hr) && *ppv == static_cast<void*>(m_real)) {
        *ppv = static_cast<IDirect3DDevice9*>(this);
    }
    if (m_diag_qi <= 16) {
        logf("DeviceWrap::QI result hr=0x%08lx ppv=%p",
             (unsigned long)hr, ppv ? *ppv : nullptr);
    }
    return hr;
}
ULONG __stdcall DeviceWrap::AddRef()  {
    if (m_diag_addref < 10) {
        ++m_diag_addref;
        ULONG c = m_real->AddRef();
        logf("DeviceWrap::AddRef -> %lu", (unsigned long)c);
        return c;
    }
    return m_real->AddRef();
}
ULONG __stdcall DeviceWrap::Release() {
    ULONG c = m_real->Release();
    if (m_diag_release < 10) {
        ++m_diag_release;
        logf("DeviceWrap::Release -> %lu", (unsigned long)c);
    }
    if (c == 0) delete this;
    return c;
}

// =====================================================================
//   Pass-through: the boring 90%.
// =====================================================================

// Macros for terseness in the pass-through block.
#define FWD(ret, name, params, args) \
    ret __stdcall DeviceWrap::name params { return m_real->name args; }
#define FWD_V(name, params, args) \
    void __stdcall DeviceWrap::name params { m_real->name args; }

FWD(HRESULT, TestCooperativeLevel, (), ())
FWD(UINT,    GetAvailableTextureMem, (), ())
FWD(HRESULT, EvictManagedResources, (), ())
HRESULT __stdcall DeviceWrap::GetDirect3D(IDirect3D9** ppD3D9) {
    HRESULT hr = m_real->GetDirect3D(ppD3D9);
    if (SUCCEEDED(hr) && ppD3D9 && *ppD3D9) {
        (*ppD3D9)->Release();                    // release the real
        m_parent->AddRef();
        *ppD3D9 = m_parent;                      // return our wrapper
    }
    return hr;
}
FWD(HRESULT, GetDeviceCaps, (D3DCAPS9* p), (p))
FWD(HRESULT, GetDisplayMode, (UINT i, D3DDISPLAYMODE* p), (i, p))
FWD(HRESULT, GetCreationParameters, (D3DDEVICE_CREATION_PARAMETERS* p), (p))
FWD(HRESULT, SetCursorProperties, (UINT x, UINT y, IDirect3DSurface9* s), (x, y, s))
FWD_V(SetCursorPosition, (int x, int y, DWORD f), (x, y, f))
FWD(BOOL,    ShowCursor, (BOOL b), (b))
FWD(HRESULT, CreateAdditionalSwapChain, (D3DPRESENT_PARAMETERS* p, IDirect3DSwapChain9** s), (p, s))
FWD(HRESULT, GetSwapChain, (UINT i, IDirect3DSwapChain9** s), (i, s))
FWD(UINT,    GetNumberOfSwapChains, (), ())
HRESULT __stdcall DeviceWrap::Reset(D3DPRESENT_PARAMETERS* pp) {
    ++m_diag_reset;
    logf("DeviceWrap::Reset #%d this=%p", m_diag_reset, (void*)this);
    HRESULT hr = m_real->Reset(pp);
    if (SUCCEEDED(hr) && pp) {
        m_bb_w = pp->BackBufferWidth;
        m_bb_h = pp->BackBufferHeight;
        logf("Reset: back buffer %ux%u", m_bb_w, m_bb_h);
    }
    return hr;
}
FWD(HRESULT, GetBackBuffer, (UINT a, UINT b, D3DBACKBUFFER_TYPE t, IDirect3DSurface9** s), (a, b, t, s))
FWD(HRESULT, GetRasterStatus, (UINT i, D3DRASTER_STATUS* p), (i, p))
FWD(HRESULT, SetDialogBoxMode, (BOOL b), (b))
FWD_V(SetGammaRamp, (UINT i, DWORD f, CONST D3DGAMMARAMP* r), (i, f, r))
FWD_V(GetGammaRamp, (UINT i, D3DGAMMARAMP* r), (i, r))
FWD(HRESULT, CreateTexture, (UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* hs), (w, h, l, u, f, p, t, hs))
FWD(HRESULT, CreateVolumeTexture, (UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** t, HANDLE* hs), (w, h, d, l, u, f, p, t, hs))
FWD(HRESULT, CreateCubeTexture, (UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** t, HANDLE* hs), (e, l, u, f, p, t, hs))
FWD(HRESULT, CreateVertexBuffer, (UINT L, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** v, HANDLE* hs), (L, u, fvf, p, v, hs))
FWD(HRESULT, CreateIndexBuffer, (UINT L, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** v, HANDLE* hs), (L, u, f, p, v, hs))
FWD(HRESULT, CreateRenderTarget, (UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE mt, DWORD mq, BOOL lk, IDirect3DSurface9** s, HANDLE* hs), (w, h, f, mt, mq, lk, s, hs))
FWD(HRESULT, CreateDepthStencilSurface, (UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE mt, DWORD mq, BOOL disc, IDirect3DSurface9** s, HANDLE* hs), (w, h, f, mt, mq, disc, s, hs))
FWD(HRESULT, UpdateSurface, (IDirect3DSurface9* a, CONST RECT* b, IDirect3DSurface9* c, CONST POINT* d), (a, b, c, d))
FWD(HRESULT, UpdateTexture, (IDirect3DBaseTexture9* a, IDirect3DBaseTexture9* b), (a, b))
FWD(HRESULT, GetRenderTargetData, (IDirect3DSurface9* a, IDirect3DSurface9* b), (a, b))
FWD(HRESULT, GetFrontBufferData, (UINT a, IDirect3DSurface9* b), (a, b))
FWD(HRESULT, StretchRect, (IDirect3DSurface9* a, CONST RECT* b, IDirect3DSurface9* c, CONST RECT* d, D3DTEXTUREFILTERTYPE f), (a, b, c, d, f))
FWD(HRESULT, ColorFill, (IDirect3DSurface9* s, CONST RECT* r, D3DCOLOR c), (s, r, c))
FWD(HRESULT, CreateOffscreenPlainSurface, (UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** s, HANDLE* hs), (w, h, f, p, s, hs))
HRESULT __stdcall DeviceWrap::SetRenderTarget(DWORD i, IDirect3DSurface9* s) {
    if (m_diag_setrt < 5) {
        ++m_diag_setrt;
        logf("DeviceWrap::SetRenderTarget #%d index=%lu surface=%p",
             m_diag_setrt, (unsigned long)i, (void*)s);
    }
    return m_real->SetRenderTarget(i, s);
}
FWD(HRESULT, GetRenderTarget, (DWORD i, IDirect3DSurface9** s), (i, s))
FWD(HRESULT, SetDepthStencilSurface, (IDirect3DSurface9* s), (s))
FWD(HRESULT, GetDepthStencilSurface, (IDirect3DSurface9** s), (s))
HRESULT __stdcall DeviceWrap::BeginScene() {
    if (m_diag_beginscene < 3) {
        ++m_diag_beginscene;
        logf("DeviceWrap::BeginScene #%d this=%p", m_diag_beginscene, (void*)this);
    }
    // Frame-boundary bookkeeping (IW3 presents via swapchain, so Present is
    // unreliable as the frame hook). BeginScene is called every frame.
    refresh_hotkey();
    ++m_frame_count;
    log_stats();
    // Start each frame assuming world; the usual CoD4 flow is
    //   world(full depth) -> viewmodel(shrunk depth) -> HUD(ortho/ZENABLE=FALSE)
    // so World is the right default. Transitions below will move us to
    // ViewModel (on shrunk viewport) and HUD2D (on ortho/ZENABLE=FALSE).
    m_pass = PassType::World;
    return m_real->BeginScene();
}
FWD(HRESULT, EndScene, (), ())
HRESULT __stdcall DeviceWrap::Clear(DWORD c, CONST D3DRECT* r, DWORD f, D3DCOLOR col, float z, DWORD s) {
    if (m_diag_clear < 3) {
        ++m_diag_clear;
        logf("DeviceWrap::Clear #%d flags=0x%lx", m_diag_clear, (unsigned long)f);
    }
    return m_real->Clear(c, r, f, col, z, s);
}
FWD(HRESULT, SetTransform, (D3DTRANSFORMSTATETYPE t, CONST D3DMATRIX* m), (t, m))
FWD(HRESULT, GetTransform, (D3DTRANSFORMSTATETYPE t, D3DMATRIX* m), (t, m))
FWD(HRESULT, MultiplyTransform, (D3DTRANSFORMSTATETYPE t, CONST D3DMATRIX* m), (t, m))
FWD(HRESULT, GetViewport, (D3DVIEWPORT9* p), (p))
FWD(HRESULT, SetMaterial, (CONST D3DMATERIAL9* p), (p))
FWD(HRESULT, GetMaterial, (D3DMATERIAL9* p), (p))
FWD(HRESULT, SetLight, (DWORD i, CONST D3DLIGHT9* l), (i, l))
FWD(HRESULT, GetLight, (DWORD i, D3DLIGHT9* l), (i, l))
FWD(HRESULT, LightEnable, (DWORD i, BOOL b), (i, b))
FWD(HRESULT, GetLightEnable, (DWORD i, BOOL* b), (i, b))
FWD(HRESULT, SetClipPlane, (DWORD i, CONST float* p), (i, p))
FWD(HRESULT, GetClipPlane, (DWORD i, float* p), (i, p))
FWD(HRESULT, GetRenderState, (D3DRENDERSTATETYPE s, DWORD* v), (s, v))
FWD(HRESULT, CreateStateBlock, (D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** p), (t, p))
FWD(HRESULT, BeginStateBlock, (), ())
FWD(HRESULT, EndStateBlock, (IDirect3DStateBlock9** p), (p))
FWD(HRESULT, SetClipStatus, (CONST D3DCLIPSTATUS9* s), (s))
FWD(HRESULT, GetClipStatus, (D3DCLIPSTATUS9* s), (s))
FWD(HRESULT, GetTexture, (DWORD s, IDirect3DBaseTexture9** t), (s, t))
FWD(HRESULT, SetTexture, (DWORD s, IDirect3DBaseTexture9* t), (s, t))
FWD(HRESULT, GetTextureStageState, (DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD* v), (s, t, v))
FWD(HRESULT, SetTextureStageState, (DWORD s, D3DTEXTURESTAGESTATETYPE t, DWORD v), (s, t, v))
FWD(HRESULT, GetSamplerState, (DWORD s, D3DSAMPLERSTATETYPE t, DWORD* v), (s, t, v))
FWD(HRESULT, SetSamplerState, (DWORD s, D3DSAMPLERSTATETYPE t, DWORD v), (s, t, v))
FWD(HRESULT, ValidateDevice, (DWORD* p), (p))
FWD(HRESULT, SetPaletteEntries, (UINT i, CONST PALETTEENTRY* p), (i, p))
FWD(HRESULT, GetPaletteEntries, (UINT i, PALETTEENTRY* p), (i, p))
FWD(HRESULT, SetCurrentTexturePalette, (UINT i), (i))
FWD(HRESULT, GetCurrentTexturePalette, (UINT* i), (i))
FWD(HRESULT, SetScissorRect, (CONST RECT* r), (r))
FWD(HRESULT, GetScissorRect, (RECT* r), (r))
FWD(HRESULT, SetSoftwareVertexProcessing, (BOOL b), (b))
FWD(BOOL,    GetSoftwareVertexProcessing, (), ())
FWD(HRESULT, SetNPatchMode, (float f), (f))
FWD(float,   GetNPatchMode, (), ())
FWD(HRESULT, ProcessVertices, (UINT a, UINT b, UINT c, IDirect3DVertexBuffer9* d, IDirect3DVertexDeclaration9* e, DWORD f), (a, b, c, d, e, f))
FWD(HRESULT, CreateVertexDeclaration, (CONST D3DVERTEXELEMENT9* e, IDirect3DVertexDeclaration9** d), (e, d))
FWD(HRESULT, SetVertexDeclaration, (IDirect3DVertexDeclaration9* d), (d))
FWD(HRESULT, GetVertexDeclaration, (IDirect3DVertexDeclaration9** d), (d))
FWD(HRESULT, SetFVF, (DWORD f), (f))
FWD(HRESULT, GetFVF, (DWORD* f), (f))
FWD(HRESULT, CreateVertexShader, (CONST DWORD* f, IDirect3DVertexShader9** s), (f, s))
FWD(HRESULT, SetVertexShader, (IDirect3DVertexShader9* s), (s))
FWD(HRESULT, GetVertexShader, (IDirect3DVertexShader9** s), (s))
FWD(HRESULT, GetVertexShaderConstantF, (UINT r, float* d, UINT c), (r, d, c))
FWD(HRESULT, SetVertexShaderConstantI, (UINT r, CONST int* d, UINT c), (r, d, c))
FWD(HRESULT, GetVertexShaderConstantI, (UINT r, int* d, UINT c), (r, d, c))
FWD(HRESULT, SetVertexShaderConstantB, (UINT r, CONST BOOL* d, UINT c), (r, d, c))
FWD(HRESULT, GetVertexShaderConstantB, (UINT r, BOOL* d, UINT c), (r, d, c))
FWD(HRESULT, SetStreamSource, (UINT i, IDirect3DVertexBuffer9* b, UINT o, UINT s), (i, b, o, s))
FWD(HRESULT, GetStreamSource, (UINT i, IDirect3DVertexBuffer9** b, UINT* o, UINT* s), (i, b, o, s))
FWD(HRESULT, SetStreamSourceFreq, (UINT i, UINT f), (i, f))
FWD(HRESULT, GetStreamSourceFreq, (UINT i, UINT* f), (i, f))
FWD(HRESULT, SetIndices, (IDirect3DIndexBuffer9* b), (b))
FWD(HRESULT, GetIndices, (IDirect3DIndexBuffer9** b), (b))
FWD(HRESULT, CreatePixelShader, (CONST DWORD* f, IDirect3DPixelShader9** s), (f, s))
FWD(HRESULT, SetPixelShader, (IDirect3DPixelShader9* s), (s))
FWD(HRESULT, GetPixelShader, (IDirect3DPixelShader9** s), (s))
FWD(HRESULT, SetPixelShaderConstantF, (UINT r, CONST float* d, UINT c), (r, d, c))
FWD(HRESULT, GetPixelShaderConstantF, (UINT r, float* d, UINT c), (r, d, c))
FWD(HRESULT, SetPixelShaderConstantI, (UINT r, CONST int* d, UINT c), (r, d, c))
FWD(HRESULT, GetPixelShaderConstantI, (UINT r, int* d, UINT c), (r, d, c))
FWD(HRESULT, SetPixelShaderConstantB, (UINT r, CONST BOOL* d, UINT c), (r, d, c))
FWD(HRESULT, GetPixelShaderConstantB, (UINT r, BOOL* d, UINT c), (r, d, c))
FWD(HRESULT, DrawRectPatch, (UINT h, CONST float* n, CONST D3DRECTPATCH_INFO* i), (h, n, i))
FWD(HRESULT, DrawTriPatch, (UINT h, CONST float* n, CONST D3DTRIPATCH_INFO* i), (h, n, i))
FWD(HRESULT, DeletePatch, (UINT h), (h))
FWD(HRESULT, CreateQuery, (D3DQUERYTYPE t, IDirect3DQuery9** q), (t, q))

#undef FWD
#undef FWD_V

// =====================================================================
//   Actual flip logic
// =====================================================================

void DeviceWrap::refresh_hotkey() {
    if (g_cfg.toggle_vk == 0) return;
    const SHORT s = GetAsyncKeyState(g_cfg.toggle_vk);
    const bool down = (s & 0x8000) != 0;
    if (down && !m_last_key_down) {
        m_runtime_enabled = !m_runtime_enabled;
        logf("hotkey: runtime_enabled -> %d", (int)m_runtime_enabled);
    }
    m_last_key_down = down;
}

void DeviceWrap::log_stats() {
    const uint64_t now = GetTickCount();
    if (m_last_stats_ticks == 0) m_last_stats_ticks = now;
    if (now - m_last_stats_ticks >= 2000) {
        logf("stats: frames=%llu world_draws=%llu vm_draws=%llu hud_draws=%llu flips=%llu (enabled=%d)",
             (unsigned long long)m_frame_count,
             (unsigned long long)m_draws_world,
             (unsigned long long)m_draws_vm,
             (unsigned long long)m_draws_hud,
             (unsigned long long)m_total_flips,
             (int)m_runtime_enabled);
        m_draws_world = m_draws_vm = m_draws_hud = 0;
        m_last_stats_ticks = now;
    }
}

HRESULT __stdcall DeviceWrap::Present(CONST RECT* a, CONST RECT* b, HWND w, CONST RGNDATA* r) {
    if (m_diag_present < 3) {
        ++m_diag_present;
        logf("DeviceWrap::Present #%d this=%p", m_diag_present, (void*)this);
    }
    // NOTE: frame-boundary bookkeeping lives in BeginScene now, because CoD4
    // presents via IDirect3DSwapChain9 and this method is rarely hit.
    return m_real->Present(a, b, w, r);
}

HRESULT __stdcall DeviceWrap::SetViewport(CONST D3DVIEWPORT9* pViewport) {
    if (pViewport && m_diag_setviewport < 5) {
        ++m_diag_setviewport;
        logf("DeviceWrap::SetViewport #%d x=%u y=%u w=%u h=%u MinZ=%.4f MaxZ=%.4f",
             m_diag_setviewport,
             pViewport->X, pViewport->Y, pViewport->Width, pViewport->Height,
             pViewport->MinZ, pViewport->MaxZ);
    }
    if (pViewport) {
        m_viewport = *pViewport;
        m_viewport_valid = true;

        // CoD4 alternates viewports every frame:
        //   shrunk depth (e.g. MinZ=0, MaxZ=~0.016)  -> viewmodel pass
        //   full   depth (MinZ=~0.016, MaxZ=1.0)    -> world/HUD pass
        // So flip back and forth.  Do NOT stomp HUD2D once we've set it
        // via ZENABLE=FALSE or via an orthographic projection detection,
        // because switching RTs for UI does not toggle the viewport back.
        const float range = pViewport->MaxZ - pViewport->MinZ;
        const PassType old_pass = m_pass;
        if (range < 0.9f) {
            m_pass = PassType::ViewModel;
        } else {
            if (m_pass == PassType::ViewModel) {
                m_pass = PassType::World;
            }
        }

        // v8: CoD4 doesn't re-upload the projection/VP matrix when entering
        // the viewmodel pass - it just shrinks the depth range and reuses
        // the world VP. So we manually push a flipped copy on entry, and
        // restore the cached (unflipped) copy on exit.  This is what
        // actually mirrors the weapon+hands geometry.
        if (m_runtime_enabled && m_vp_cache_valid) {
            if (m_pass == PassType::ViewModel && old_pass != PassType::ViewModel) {
                // Entering VM -> push flipped VP.
                if (!m_vp_is_flipped_on_device) {
                    float flipped[16];
                    std::memcpy(flipped, m_vp_cache, sizeof(flipped));
                    flip_column0(flipped);
                    m_real->SetVertexShaderConstantF(m_vp_cache_reg, flipped, 4);
                    m_vp_is_flipped_on_device = true;
                    ++m_total_flips;
                }
            } else if (old_pass == PassType::ViewModel && m_pass != PassType::ViewModel) {
                // Leaving VM -> restore unflipped VP.
                if (m_vp_is_flipped_on_device) {
                    m_real->SetVertexShaderConstantF(m_vp_cache_reg, m_vp_cache, 4);
                    m_vp_is_flipped_on_device = false;
                }
            }
        }
    }
    return m_real->SetViewport(pViewport);
}

HRESULT __stdcall DeviceWrap::SetRenderState(D3DRENDERSTATETYPE state, DWORD value) {
    if (state == D3DRS_CULLMODE) {
        m_engine_cullmode = value;
        // v8: swap cullmode only when the VP currently on device is actually
        // flipped (not just when we're in ViewModel pass).  This keeps
        // culling correct when CoD4 re-uses the unflipped world VP.
        if (m_vp_is_flipped_on_device) {
            DWORD swapped = value;
            if (value == D3DCULL_CW)      swapped = D3DCULL_CCW;
            else if (value == D3DCULL_CCW) swapped = D3DCULL_CW;
            m_device_cullmode = swapped;
            return m_real->SetRenderState(state, swapped);
        }
        m_device_cullmode = value;
    }
    if (state == D3DRS_ZENABLE) {
        // ZENABLE == FALSE is almost always the 2D HUD pass.
        if (value == D3DZB_FALSE) {
            m_pass = PassType::HUD2D;
        }
    }
    return m_real->SetRenderState(state, value);
}

HRESULT __stdcall DeviceWrap::SetVertexShaderConstantF(
        UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) {
    if (!pConstantData || Vector4fCount < 4) {
        return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
    }

    const UINT total_floats = Vector4fCount * 4;

    // Phase 1: classify and cache.  When a matrix with a projection-like
    // shape passes through, cache an UNFLIPPED copy - we need it later to
    // restore device state when leaving the ViewModel pass (CoD4 does not
    // re-upload the VP for the VM sub-pass, it simply shrinks the depth
    // range, so we have to mirror the cached copy ourselves).
    for (UINT base = 0; base + 16 <= total_floats; base += 4) {
        if ((base & 15) != 0) continue;
        const float* sub = pConstantData + base;
        const bool proj = looks_like_pure_projection(sub);
        const bool vp   = looks_like_view_projection(sub);
        if (proj) {
            if (m_pass != PassType::HUD2D && m_pass != PassType::ViewModel) {
                m_pass = PassType::World;
            }
        }
        if (proj || vp) {
            const UINT reg = StartRegister + base / 4;
            std::memcpy(m_vp_cache, sub, sizeof(m_vp_cache));
            m_vp_cache_reg   = reg;
            m_vp_cache_valid = true;
            // An engine-driven upload during a non-VM pass always leaves
            // the device in the unflipped state (the data the engine
            // asked for is on the device).
            if (m_pass != PassType::ViewModel) {
                m_vp_is_flipped_on_device = false;
            }
        }
    }

    if (!m_runtime_enabled) {
        return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
    }

    // Phase 2: if we happen to be on the VM pass AND the engine IS re-
    // uploading a projection-shaped matrix (rare but possible - some CoD4
    // shaders rebind constants), flip on the fly in a local buffer before
    // the upload reaches the device.
    bool any_flipped = false;
    float tmp_stack[256];
    float* mutated = nullptr;

    for (UINT base = 0; base + 16 <= total_floats; base += 4) {
        if ((base & 15) != 0) continue;
        const float* sub = pConstantData + base;

        if (m_pass != PassType::ViewModel) continue;

        bool want = false;
        if (looks_like_pure_projection(sub)) {
            want = true;
        } else if (looks_like_view_projection(sub)) {
            want = true;
        }
        if (!want) continue;

        if (!mutated) {
            if (total_floats <= sizeof(tmp_stack) / sizeof(tmp_stack[0])) {
                mutated = tmp_stack;
            } else {
                mutated = new float[total_floats];
            }
            std::memcpy(mutated, pConstantData, total_floats * sizeof(float));
        }
        flip_column0(mutated + base);
        any_flipped = true;
        m_vp_is_flipped_on_device = true;
        ++m_total_flips;
    }

    HRESULT hr = m_real->SetVertexShaderConstantF(
        StartRegister,
        any_flipped ? mutated : pConstantData,
        Vector4fCount);

    if (mutated && mutated != tmp_stack) delete[] mutated;
    return hr;
}

bool DeviceWrap::pre_draw() {
    // v8: On every draw, ensure the cullmode on the device matches the
    // current "is the VP flipped?" state - not the pass type.  When the VP
    // was NOT re-uploaded for this pass (typical CoD4 VM pass) and we
    // haven't injected a flipped copy yet, cullmode must stay unswapped
    // or we render inside-out back-faces.
    const bool active = (m_runtime_enabled && m_vp_is_flipped_on_device);
    DWORD wanted = m_engine_cullmode;
    if (active) {
        if (m_engine_cullmode == D3DCULL_CW)      wanted = D3DCULL_CCW;
        else if (m_engine_cullmode == D3DCULL_CCW) wanted = D3DCULL_CW;
    }
    if (wanted != m_device_cullmode) {
        m_real->SetRenderState(D3DRS_CULLMODE, wanted);
        m_device_cullmode = wanted;
    }
    return active;
}
void DeviceWrap::post_draw(bool) {
    switch (m_pass) {
        case PassType::World:     ++m_draws_world; break;
        case PassType::ViewModel: ++m_draws_vm;    break;
        case PassType::HUD2D:     ++m_draws_hud;   break;
        default: break;
    }
}

HRESULT __stdcall DeviceWrap::DrawPrimitive(
        D3DPRIMITIVETYPE pt, UINT start, UINT count) {
    if (m_diag_drawprim < 3) {
        ++m_diag_drawprim;
        logf("DeviceWrap::DrawPrimitive #%d pass=%d pt=%d start=%u count=%u",
             m_diag_drawprim, (int)m_pass, (int)pt, start, count);
    }
    const bool f = pre_draw();
    HRESULT hr = m_real->DrawPrimitive(pt, start, count);
    post_draw(f);
    return hr;
}
HRESULT __stdcall DeviceWrap::DrawIndexedPrimitive(
        D3DPRIMITIVETYPE pt, INT bvi, UINT mvi, UINT nv, UINT si, UINT pc) {
    if (m_diag_drawindexed < 3) {
        ++m_diag_drawindexed;
        logf("DeviceWrap::DrawIndexedPrimitive #%d pass=%d nv=%u pc=%u",
             m_diag_drawindexed, (int)m_pass, nv, pc);
    }
    const bool f = pre_draw();
    HRESULT hr = m_real->DrawIndexedPrimitive(pt, bvi, mvi, nv, si, pc);
    post_draw(f);
    return hr;
}
HRESULT __stdcall DeviceWrap::DrawPrimitiveUP(
        D3DPRIMITIVETYPE pt, UINT pc, CONST void* vd, UINT s) {
    if (m_diag_drawprimup < 3) {
        ++m_diag_drawprimup;
        logf("DeviceWrap::DrawPrimitiveUP #%d pass=%d pc=%u",
             m_diag_drawprimup, (int)m_pass, pc);
    }
    const bool f = pre_draw();
    HRESULT hr = m_real->DrawPrimitiveUP(pt, pc, vd, s);
    post_draw(f);
    return hr;
}
HRESULT __stdcall DeviceWrap::DrawIndexedPrimitiveUP(
        D3DPRIMITIVETYPE pt, UINT mvi, UINT nv, UINT pc,
        CONST void* idx, D3DFORMAT ifmt, CONST void* vd, UINT s) {
    if (m_diag_drawindexedup < 3) {
        ++m_diag_drawindexedup;
        logf("DeviceWrap::DrawIndexedPrimitiveUP #%d pass=%d pc=%u",
             m_diag_drawindexedup, (int)m_pass, pc);
    }
    const bool f = pre_draw();
    HRESULT hr = m_real->DrawIndexedPrimitiveUP(pt, mvi, nv, pc, idx, ifmt, vd, s);
    post_draw(f);
    return hr;
}

} // namespace wf

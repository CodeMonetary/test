#pragma once

#include <d3d9.h>
#include <cstdint>

namespace wf {

class D3D9Wrap;

enum class PassType : int {
    Unknown    = 0,
    World      = 1,  // perspective projection, full depth range
    ViewModel  = 2,  // perspective projection, shrunk depth range (MinZ..MaxZ != 0..1)
    HUD2D      = 3,  // orthographic projection
};

class DeviceWrap final : public IDirect3DDevice9 {
public:
    DeviceWrap(IDirect3DDevice9* real, D3D9Wrap* parent);
    ~DeviceWrap() {}

    // IUnknown
    HRESULT __stdcall QueryInterface(REFIID riid, void** ppv) override;
    ULONG   __stdcall AddRef() override;
    ULONG   __stdcall Release() override;

    // IDirect3DDevice9 (full surface area - most are pure pass-through)
    HRESULT __stdcall TestCooperativeLevel() override;
    UINT    __stdcall GetAvailableTextureMem() override;
    HRESULT __stdcall EvictManagedResources() override;
    HRESULT __stdcall GetDirect3D(IDirect3D9** ppD3D9) override;
    HRESULT __stdcall GetDeviceCaps(D3DCAPS9*) override;
    HRESULT __stdcall GetDisplayMode(UINT, D3DDISPLAYMODE*) override;
    HRESULT __stdcall GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS*) override;
    HRESULT __stdcall SetCursorProperties(UINT, UINT, IDirect3DSurface9*) override;
    void    __stdcall SetCursorPosition(int, int, DWORD) override;
    BOOL    __stdcall ShowCursor(BOOL) override;
    HRESULT __stdcall CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain9**) override;
    HRESULT __stdcall GetSwapChain(UINT, IDirect3DSwapChain9**) override;
    UINT    __stdcall GetNumberOfSwapChains() override;
    HRESULT __stdcall Reset(D3DPRESENT_PARAMETERS*) override;
    HRESULT __stdcall Present(CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*) override;
    HRESULT __stdcall GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**) override;
    HRESULT __stdcall GetRasterStatus(UINT, D3DRASTER_STATUS*) override;
    HRESULT __stdcall SetDialogBoxMode(BOOL) override;
    void    __stdcall SetGammaRamp(UINT, DWORD, CONST D3DGAMMARAMP*) override;
    void    __stdcall GetGammaRamp(UINT, D3DGAMMARAMP*) override;
    HRESULT __stdcall CreateTexture(UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*) override;
    HRESULT __stdcall CreateVolumeTexture(UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*) override;
    HRESULT __stdcall CreateCubeTexture(UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*) override;
    HRESULT __stdcall CreateVertexBuffer(UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*) override;
    HRESULT __stdcall CreateIndexBuffer(UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*) override;
    HRESULT __stdcall CreateRenderTarget(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) override;
    HRESULT __stdcall CreateDepthStencilSurface(UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) override;
    HRESULT __stdcall UpdateSurface(IDirect3DSurface9*, CONST RECT*, IDirect3DSurface9*, CONST POINT*) override;
    HRESULT __stdcall UpdateTexture(IDirect3DBaseTexture9*, IDirect3DBaseTexture9*) override;
    HRESULT __stdcall GetRenderTargetData(IDirect3DSurface9*, IDirect3DSurface9*) override;
    HRESULT __stdcall GetFrontBufferData(UINT, IDirect3DSurface9*) override;
    HRESULT __stdcall StretchRect(IDirect3DSurface9*, CONST RECT*, IDirect3DSurface9*, CONST RECT*, D3DTEXTUREFILTERTYPE) override;
    HRESULT __stdcall ColorFill(IDirect3DSurface9*, CONST RECT*, D3DCOLOR) override;
    HRESULT __stdcall CreateOffscreenPlainSurface(UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*) override;
    HRESULT __stdcall SetRenderTarget(DWORD, IDirect3DSurface9*) override;
    HRESULT __stdcall GetRenderTarget(DWORD, IDirect3DSurface9**) override;
    HRESULT __stdcall SetDepthStencilSurface(IDirect3DSurface9*) override;
    HRESULT __stdcall GetDepthStencilSurface(IDirect3DSurface9**) override;
    HRESULT __stdcall BeginScene() override;
    HRESULT __stdcall EndScene() override;
    HRESULT __stdcall Clear(DWORD, CONST D3DRECT*, DWORD, D3DCOLOR, float, DWORD) override;
    HRESULT __stdcall SetTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*) override;
    HRESULT __stdcall GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX*) override;
    HRESULT __stdcall MultiplyTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*) override;
    HRESULT __stdcall SetViewport(CONST D3DVIEWPORT9*) override;
    HRESULT __stdcall GetViewport(D3DVIEWPORT9*) override;
    HRESULT __stdcall SetMaterial(CONST D3DMATERIAL9*) override;
    HRESULT __stdcall GetMaterial(D3DMATERIAL9*) override;
    HRESULT __stdcall SetLight(DWORD, CONST D3DLIGHT9*) override;
    HRESULT __stdcall GetLight(DWORD, D3DLIGHT9*) override;
    HRESULT __stdcall LightEnable(DWORD, BOOL) override;
    HRESULT __stdcall GetLightEnable(DWORD, BOOL*) override;
    HRESULT __stdcall SetClipPlane(DWORD, CONST float*) override;
    HRESULT __stdcall GetClipPlane(DWORD, float*) override;
    HRESULT __stdcall SetRenderState(D3DRENDERSTATETYPE, DWORD) override;
    HRESULT __stdcall GetRenderState(D3DRENDERSTATETYPE, DWORD*) override;
    HRESULT __stdcall CreateStateBlock(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**) override;
    HRESULT __stdcall BeginStateBlock() override;
    HRESULT __stdcall EndStateBlock(IDirect3DStateBlock9**) override;
    HRESULT __stdcall SetClipStatus(CONST D3DCLIPSTATUS9*) override;
    HRESULT __stdcall GetClipStatus(D3DCLIPSTATUS9*) override;
    HRESULT __stdcall GetTexture(DWORD, IDirect3DBaseTexture9**) override;
    HRESULT __stdcall SetTexture(DWORD, IDirect3DBaseTexture9*) override;
    HRESULT __stdcall GetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD*) override;
    HRESULT __stdcall SetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD) override;
    HRESULT __stdcall GetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD*) override;
    HRESULT __stdcall SetSamplerState(DWORD, D3DSAMPLERSTATETYPE, DWORD) override;
    HRESULT __stdcall ValidateDevice(DWORD*) override;
    HRESULT __stdcall SetPaletteEntries(UINT, CONST PALETTEENTRY*) override;
    HRESULT __stdcall GetPaletteEntries(UINT, PALETTEENTRY*) override;
    HRESULT __stdcall SetCurrentTexturePalette(UINT) override;
    HRESULT __stdcall GetCurrentTexturePalette(UINT*) override;
    HRESULT __stdcall SetScissorRect(CONST RECT*) override;
    HRESULT __stdcall GetScissorRect(RECT*) override;
    HRESULT __stdcall SetSoftwareVertexProcessing(BOOL) override;
    BOOL    __stdcall GetSoftwareVertexProcessing() override;
    HRESULT __stdcall SetNPatchMode(float) override;
    float   __stdcall GetNPatchMode() override;
    HRESULT __stdcall DrawPrimitive(D3DPRIMITIVETYPE, UINT, UINT) override;
    HRESULT __stdcall DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) override;
    HRESULT __stdcall DrawPrimitiveUP(D3DPRIMITIVETYPE, UINT, CONST void*, UINT) override;
    HRESULT __stdcall DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE, UINT, UINT, UINT, CONST void*, D3DFORMAT, CONST void*, UINT) override;
    HRESULT __stdcall ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer9*, IDirect3DVertexDeclaration9*, DWORD) override;
    HRESULT __stdcall CreateVertexDeclaration(CONST D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**) override;
    HRESULT __stdcall SetVertexDeclaration(IDirect3DVertexDeclaration9*) override;
    HRESULT __stdcall GetVertexDeclaration(IDirect3DVertexDeclaration9**) override;
    HRESULT __stdcall SetFVF(DWORD) override;
    HRESULT __stdcall GetFVF(DWORD*) override;
    HRESULT __stdcall CreateVertexShader(CONST DWORD*, IDirect3DVertexShader9**) override;
    HRESULT __stdcall SetVertexShader(IDirect3DVertexShader9*) override;
    HRESULT __stdcall GetVertexShader(IDirect3DVertexShader9**) override;
    HRESULT __stdcall SetVertexShaderConstantF(UINT, CONST float*, UINT) override;
    HRESULT __stdcall GetVertexShaderConstantF(UINT, float*, UINT) override;
    HRESULT __stdcall SetVertexShaderConstantI(UINT, CONST int*, UINT) override;
    HRESULT __stdcall GetVertexShaderConstantI(UINT, int*, UINT) override;
    HRESULT __stdcall SetVertexShaderConstantB(UINT, CONST BOOL*, UINT) override;
    HRESULT __stdcall GetVertexShaderConstantB(UINT, BOOL*, UINT) override;
    HRESULT __stdcall SetStreamSource(UINT, IDirect3DVertexBuffer9*, UINT, UINT) override;
    HRESULT __stdcall GetStreamSource(UINT, IDirect3DVertexBuffer9**, UINT*, UINT*) override;
    HRESULT __stdcall SetStreamSourceFreq(UINT, UINT) override;
    HRESULT __stdcall GetStreamSourceFreq(UINT, UINT*) override;
    HRESULT __stdcall SetIndices(IDirect3DIndexBuffer9*) override;
    HRESULT __stdcall GetIndices(IDirect3DIndexBuffer9**) override;
    HRESULT __stdcall CreatePixelShader(CONST DWORD*, IDirect3DPixelShader9**) override;
    HRESULT __stdcall SetPixelShader(IDirect3DPixelShader9*) override;
    HRESULT __stdcall GetPixelShader(IDirect3DPixelShader9**) override;
    HRESULT __stdcall SetPixelShaderConstantF(UINT, CONST float*, UINT) override;
    HRESULT __stdcall GetPixelShaderConstantF(UINT, float*, UINT) override;
    HRESULT __stdcall SetPixelShaderConstantI(UINT, CONST int*, UINT) override;
    HRESULT __stdcall GetPixelShaderConstantI(UINT, int*, UINT) override;
    HRESULT __stdcall SetPixelShaderConstantB(UINT, CONST BOOL*, UINT) override;
    HRESULT __stdcall GetPixelShaderConstantB(UINT, BOOL*, UINT) override;
    HRESULT __stdcall DrawRectPatch(UINT, CONST float*, CONST D3DRECTPATCH_INFO*) override;
    HRESULT __stdcall DrawTriPatch(UINT, CONST float*, CONST D3DTRIPATCH_INFO*) override;
    HRESULT __stdcall DeletePatch(UINT) override;
    HRESULT __stdcall CreateQuery(D3DQUERYTYPE, IDirect3DQuery9**) override;

private:
    // Pass-detection / flip state -----------------------------------------
    void update_pass_classification();
    bool pre_draw();  // returns true if a flip is active; adjusts cullmode
    void post_draw(bool flipped);
    void refresh_hotkey();
    void log_stats();

    IDirect3DDevice9* m_real;
    D3D9Wrap*         m_parent;

    // current classified pass for the about-to-be-drawn primitive
    PassType m_pass = PassType::Unknown;

    // current viewport (last seen)
    D3DVIEWPORT9 m_viewport = {};
    bool         m_viewport_valid = false;

    // backbuffer dimensions (for "full screen" detection)
    UINT m_bb_w = 0;
    UINT m_bb_h = 0;

    // last projection we pushed to VS (registers 0..3 is the engine convention)
    // and its analysis
    float m_last_proj[16] = {0};
    bool  m_proj_valid = false;
    bool  m_proj_is_perspective = false;
    bool  m_proj_is_ortho = false;

    // original projection storage (so that when flipping we can restore when needed)
    // Not strictly required at the proxy level -- we can always re-derive it.

    // Current cullmode as requested by the engine.
    DWORD m_engine_cullmode = D3DCULL_CCW;
    // Cullmode actually pushed to the device (possibly swapped if a flip is active)
    DWORD m_device_cullmode = D3DCULL_CCW;

    // Stats (per second)
    uint64_t m_frame_count = 0;
    uint64_t m_draws_world = 0;
    uint64_t m_draws_vm    = 0;
    uint64_t m_draws_hud   = 0;
    uint64_t m_last_stats_ticks = 0;

    // Runtime toggle (hotkey)
    bool m_runtime_enabled = true;  // seeded from config.enabled at device creation
    bool m_last_key_down = false;

    // Diagnostic "first-N-call" counters.
    int m_diag_qi = 0;
    int m_diag_addref = 0;
    int m_diag_release = 0;
    int m_diag_present = 0;
    int m_diag_reset = 0;
    int m_diag_setviewport = 0;
    int m_diag_setvsconstf = 0;
    int m_diag_setrenderstate = 0;
    int m_diag_drawprim = 0;
    int m_diag_drawindexed = 0;
    int m_diag_drawprimup = 0;
    int m_diag_drawindexedup = 0;
    int m_diag_setrt = 0;
    int m_diag_clear = 0;
    int m_diag_beginscene = 0;
    int m_diag_flips_logged = 0;
    uint64_t m_total_flips = 0;
};

} // namespace wf

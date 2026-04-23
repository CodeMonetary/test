#include "d3d9_wrapper.hpp"
#include "device_wrapper.hpp"
#include "logger.hpp"

namespace wf {

// {02177241-69FC-400C-8FF1-93A44DF6861D} - IID_IDirect3D9Ex
static const IID IID_IDirect3D9Ex_local =
    { 0x02177241, 0x69FC, 0x400C, { 0x8F, 0xF1, 0x93, 0xA4, 0x4D, 0xF6, 0x86, 0x1D } };

HRESULT __stdcall D3D9Wrap::QueryInterface(REFIID riid, void** ppv) {
    log_guid("D3D9Wrap::QI", riid);
    // Refuse IDirect3D9Ex: if the caller gets the raw Ex, it bypasses our
    // CreateDevice wrap entirely. Force the caller onto the non-Ex path.
    if (riid == IID_IDirect3D9Ex_local) {
        if (ppv) *ppv = nullptr;
        logf("D3D9Wrap::QI declined IDirect3D9Ex");
        return E_NOINTERFACE;
    }
    HRESULT hr = m_real->QueryInterface(riid, ppv);
    if (SUCCEEDED(hr) && *ppv == static_cast<void*>(m_real)) {
        *ppv = static_cast<IDirect3D9*>(this);
    }
    logf("D3D9Wrap::QI result hr=0x%08lx ppv=%p", (unsigned long)hr, ppv ? *ppv : nullptr);
    return hr;
}
ULONG __stdcall D3D9Wrap::AddRef()  { return m_real->AddRef(); }
ULONG __stdcall D3D9Wrap::Release() {
    ULONG c = m_real->Release();
    if (c == 0) delete this;
    return c;
}

HRESULT __stdcall D3D9Wrap::RegisterSoftwareDevice(void* p)
    { return m_real->RegisterSoftwareDevice(p); }
UINT    __stdcall D3D9Wrap::GetAdapterCount()
    { return m_real->GetAdapterCount(); }
HRESULT __stdcall D3D9Wrap::GetAdapterIdentifier(UINT A, DWORD F, D3DADAPTER_IDENTIFIER9* p)
    { return m_real->GetAdapterIdentifier(A, F, p); }
UINT    __stdcall D3D9Wrap::GetAdapterModeCount(UINT A, D3DFORMAT F)
    { return m_real->GetAdapterModeCount(A, F); }
HRESULT __stdcall D3D9Wrap::EnumAdapterModes(UINT A, D3DFORMAT F, UINT M, D3DDISPLAYMODE* p)
    { return m_real->EnumAdapterModes(A, F, M, p); }
HRESULT __stdcall D3D9Wrap::GetAdapterDisplayMode(UINT A, D3DDISPLAYMODE* p)
    { return m_real->GetAdapterDisplayMode(A, p); }
HRESULT __stdcall D3D9Wrap::CheckDeviceType(UINT A, D3DDEVTYPE D, D3DFORMAT DF, D3DFORMAT BF, BOOL W)
    { return m_real->CheckDeviceType(A, D, DF, BF, W); }
HRESULT __stdcall D3D9Wrap::CheckDeviceFormat(UINT A, D3DDEVTYPE D, D3DFORMAT AF, DWORD U, D3DRESOURCETYPE R, D3DFORMAT CF)
    { return m_real->CheckDeviceFormat(A, D, AF, U, R, CF); }
HRESULT __stdcall D3D9Wrap::CheckDeviceMultiSampleType(UINT A, D3DDEVTYPE D, D3DFORMAT SF, BOOL W, D3DMULTISAMPLE_TYPE MT, DWORD* QL)
    { return m_real->CheckDeviceMultiSampleType(A, D, SF, W, MT, QL); }
HRESULT __stdcall D3D9Wrap::CheckDepthStencilMatch(UINT A, D3DDEVTYPE D, D3DFORMAT AF, D3DFORMAT RTF, D3DFORMAT DSF)
    { return m_real->CheckDepthStencilMatch(A, D, AF, RTF, DSF); }
HRESULT __stdcall D3D9Wrap::CheckDeviceFormatConversion(UINT A, D3DDEVTYPE D, D3DFORMAT S, D3DFORMAT T)
    { return m_real->CheckDeviceFormatConversion(A, D, S, T); }
HRESULT __stdcall D3D9Wrap::GetDeviceCaps(UINT A, D3DDEVTYPE D, D3DCAPS9* p)
    { return m_real->GetDeviceCaps(A, D, p); }
HMONITOR __stdcall D3D9Wrap::GetAdapterMonitor(UINT A)
    { return m_real->GetAdapterMonitor(A); }

HRESULT __stdcall D3D9Wrap::CreateDevice(
        UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
        DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
        IDirect3DDevice9** ppReturnedDeviceInterface) {
    IDirect3DDevice9* real_dev = nullptr;
    HRESULT hr = m_real->CreateDevice(Adapter, DeviceType, hFocusWindow,
                                      BehaviorFlags, pPresentationParameters,
                                      &real_dev);
    if (FAILED(hr) || !real_dev) {
        logf("CreateDevice failed hr=0x%08lx", (unsigned long)hr);
        *ppReturnedDeviceInterface = real_dev;
        return hr;
    }
    logf("CreateDevice OK: %dx%d windowed=%d adapter=%u type=%d",
         pPresentationParameters ? (int)pPresentationParameters->BackBufferWidth  : -1,
         pPresentationParameters ? (int)pPresentationParameters->BackBufferHeight : -1,
         pPresentationParameters ? (int)pPresentationParameters->Windowed         : -1,
         Adapter, (int)DeviceType);
    DeviceWrap* wrap = new DeviceWrap(real_dev, this);
    *ppReturnedDeviceInterface = wrap;
    logf("CreateDevice returned IDirect3DDevice9* = %p (real=%p)",
         (void*)wrap, (void*)real_dev);
    return hr;
}

} // namespace wf

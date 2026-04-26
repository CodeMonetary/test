// cod4mirror — IDirect3D9 wrapper passthroughs + CreateDevice override.
#include "d3d9_object.h"
#include "d3d9_device.h"

namespace cod4mirror
{
	HRESULT __stdcall D3D9Object::QueryInterface(REFIID riid, void** ppvObj)
	{
		if (ppvObj == nullptr) return E_POINTER;
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9))
		{
			AddRef();
			*ppvObj = this;
			return S_OK;
		}
		return m_orig->QueryInterface(riid, ppvObj);
	}

	ULONG __stdcall D3D9Object::AddRef()  { return m_orig->AddRef(); }
	ULONG __stdcall D3D9Object::Release()
	{
		const ULONG c = m_orig->Release();
		if (c == 0) delete this;
		return c;
	}

	HRESULT  __stdcall D3D9Object::RegisterSoftwareDevice(void* p)                                                                                                  { return m_orig->RegisterSoftwareDevice(p); }
	UINT     __stdcall D3D9Object::GetAdapterCount()                                                                                                                { return m_orig->GetAdapterCount(); }
	HRESULT  __stdcall D3D9Object::GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER9* id)                                                                { return m_orig->GetAdapterIdentifier(a, f, id); }
	UINT     __stdcall D3D9Object::GetAdapterModeCount(UINT a, D3DFORMAT fmt)                                                                                       { return m_orig->GetAdapterModeCount(a, fmt); }
	HRESULT  __stdcall D3D9Object::EnumAdapterModes(UINT a, D3DFORMAT f, UINT m, D3DDISPLAYMODE* p)                                                                 { return m_orig->EnumAdapterModes(a, f, m, p); }
	HRESULT  __stdcall D3D9Object::GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* p)                                                                                 { return m_orig->GetAdapterDisplayMode(a, p); }
	HRESULT  __stdcall D3D9Object::CheckDeviceType(UINT a, D3DDEVTYPE d, D3DFORMAT df, D3DFORMAT bf, BOOL w)                                                        { return m_orig->CheckDeviceType(a, d, df, bf, w); }
	HRESULT  __stdcall D3D9Object::CheckDeviceFormat(UINT a, D3DDEVTYPE d, D3DFORMAT af, DWORD u, D3DRESOURCETYPE r, D3DFORMAT cf)                                  { return m_orig->CheckDeviceFormat(a, d, af, u, r, cf); }
	HRESULT  __stdcall D3D9Object::CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE d, D3DFORMAT sf, BOOL w, D3DMULTISAMPLE_TYPE mt, DWORD* q)                         { return m_orig->CheckDeviceMultiSampleType(a, d, sf, w, mt, q); }
	HRESULT  __stdcall D3D9Object::CheckDepthStencilMatch(UINT a, D3DDEVTYPE d, D3DFORMAT af, D3DFORMAT rtf, D3DFORMAT dsf)                                         { return m_orig->CheckDepthStencilMatch(a, d, af, rtf, dsf); }
	HRESULT  __stdcall D3D9Object::CheckDeviceFormatConversion(UINT a, D3DDEVTYPE d, D3DFORMAT s, D3DFORMAT t)                                                      { return m_orig->CheckDeviceFormatConversion(a, d, s, t); }
	HRESULT  __stdcall D3D9Object::GetDeviceCaps(UINT a, D3DDEVTYPE d, D3DCAPS9* c)                                                                                 { return m_orig->GetDeviceCaps(a, d, c); }
	HMONITOR __stdcall D3D9Object::GetAdapterMonitor(UINT a)                                                                                                        { return m_orig->GetAdapterMonitor(a); }

	HRESULT __stdcall D3D9Object::CreateDevice(
		UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
		DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPP,
		IDirect3DDevice9** ppReturned)
	{
		if (!ppReturned) return E_POINTER;

		IDirect3DDevice9* real = nullptr;
		const HRESULT hr = m_orig->CreateDevice(Adapter, DeviceType, hFocusWindow,
			BehaviorFlags, pPP, &real);
		if (FAILED(hr) || !real)
		{
			*ppReturned = real;
			return hr;
		}

		// wrap so engine sees our hooks on every method call.
		*ppReturned = new D3D9Device(real);
		return hr;
	}
}

// cod4mirror — IDirect3DDevice9 wrapper. Most methods passthrough; a few
// hot ones (Present/EndScene/SetVertex|PixelShaderConstantF/Draw*) call into
// the mirror module.
#include "d3d9_device.h"
#include "mirror.h"

namespace cod4mirror
{
	HRESULT __stdcall D3D9Device::TestCooperativeLevel()
	{
		return m_orig->TestCooperativeLevel();
	}

	UINT __stdcall D3D9Device::GetAvailableTextureMem()
	{
		return m_orig->GetAvailableTextureMem();
	}

	HRESULT __stdcall D3D9Device::EvictManagedResources()
	{
		return m_orig->EvictManagedResources();
	}

	HRESULT __stdcall D3D9Device::GetDeviceCaps(D3DCAPS9* pCaps)
	{
		return m_orig->GetDeviceCaps(pCaps);
	}

	HRESULT __stdcall D3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode)
	{
		return m_orig->GetDisplayMode(iSwapChain, pMode);
	}

	HRESULT __stdcall D3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
	{
		return m_orig->GetCreationParameters(pParameters);
	}

	HRESULT __stdcall D3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap)
	{
		return m_orig->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
	}

	void __stdcall D3D9Device::SetCursorPosition(int X, int Y, DWORD Flags)
	{
		m_orig->SetCursorPosition(X, Y, Flags);
	}

	BOOL __stdcall D3D9Device::ShowCursor(BOOL bShow)
	{
		return m_orig->ShowCursor(bShow);
	}

	HRESULT __stdcall D3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain)
	{
		return m_orig->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
	}

	HRESULT __stdcall D3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain)
	{
		return m_orig->GetSwapChain(iSwapChain, pSwapChain);
	}

	UINT __stdcall D3D9Device::GetNumberOfSwapChains()
	{
		return m_orig->GetNumberOfSwapChains();
	}

	HRESULT __stdcall D3D9Device::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
	{
		return m_orig->Reset(pPresentationParameters);
	}

	HRESULT __stdcall D3D9Device::Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion)
	{
		mirror::on_present(m_orig);
		return m_orig->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
	}

	HRESULT __stdcall D3D9Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
	{
		return m_orig->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);
	}

	HRESULT __stdcall D3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus)
	{
		return m_orig->GetRasterStatus(iSwapChain, pRasterStatus);
	}

	HRESULT __stdcall D3D9Device::SetDialogBoxMode(BOOL bEnableDialogs)
	{
		return m_orig->SetDialogBoxMode(bEnableDialogs);
	}

	void __stdcall D3D9Device::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp)
	{
		m_orig->SetGammaRamp(iSwapChain, Flags, pRamp);
	}

	void __stdcall D3D9Device::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp)
	{
		m_orig->GetGammaRamp(iSwapChain, pRamp);
	}

	HRESULT __stdcall D3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
	{
		return m_orig->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
	}

	HRESULT __stdcall D3D9Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle)
	{
		return m_orig->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
	}

	HRESULT __stdcall D3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle)
	{
		return m_orig->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
	}

	HRESULT __stdcall D3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
	{
		return m_orig->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
	}

	HRESULT __stdcall D3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle)
	{
		return m_orig->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
	}

	HRESULT __stdcall D3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		return m_orig->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
	}

	HRESULT __stdcall D3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		return m_orig->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
	}

	HRESULT __stdcall D3D9Device::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint)
	{
		return m_orig->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint);
	}

	HRESULT __stdcall D3D9Device::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture)
	{
		return m_orig->UpdateTexture(pSourceTexture, pDestinationTexture);
	}

	HRESULT __stdcall D3D9Device::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
	{
		return m_orig->GetRenderTargetData(pRenderTarget, pDestSurface);
	}

	HRESULT __stdcall D3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface)
	{
		return m_orig->GetFrontBufferData(iSwapChain, pDestSurface);
	}

	HRESULT __stdcall D3D9Device::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
	{
		return m_orig->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
	}

	HRESULT __stdcall D3D9Device::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color)
	{
		return m_orig->ColorFill(pSurface, pRect, color);
	}

	HRESULT __stdcall D3D9Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
	{
		return m_orig->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
	}

	HRESULT __stdcall D3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
	{
		return m_orig->SetRenderTarget(RenderTargetIndex, pRenderTarget);
	}

	HRESULT __stdcall D3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget)
	{
		return m_orig->GetRenderTarget(RenderTargetIndex, ppRenderTarget);
	}

	HRESULT __stdcall D3D9Device::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
	{
		return m_orig->SetDepthStencilSurface(pNewZStencil);
	}

	HRESULT __stdcall D3D9Device::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface)
	{
		return m_orig->GetDepthStencilSurface(ppZStencilSurface);
	}

	HRESULT __stdcall D3D9Device::BeginScene()
	{
		return m_orig->BeginScene();
	}

	HRESULT __stdcall D3D9Device::EndScene()
	{
		mirror::on_end_scene(m_orig);
		return m_orig->EndScene();
	}

	HRESULT __stdcall D3D9Device::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
	{
		return m_orig->Clear(Count, pRects, Flags, Color, Z, Stencil);
	}

	HRESULT __stdcall D3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		return m_orig->SetTransform(State, pMatrix);
	}

	HRESULT __stdcall D3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix)
	{
		return m_orig->GetTransform(State, pMatrix);
	}

	HRESULT __stdcall D3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
	{
		return m_orig->MultiplyTransform(State, pMatrix);
	}

	HRESULT __stdcall D3D9Device::SetViewport(CONST D3DVIEWPORT9* pViewport)
	{
		return m_orig->SetViewport(pViewport);
	}

	HRESULT __stdcall D3D9Device::GetViewport(D3DVIEWPORT9* pViewport)
	{
		return m_orig->GetViewport(pViewport);
	}

	HRESULT __stdcall D3D9Device::SetMaterial(CONST D3DMATERIAL9* pMaterial)
	{
		return m_orig->SetMaterial(pMaterial);
	}

	HRESULT __stdcall D3D9Device::GetMaterial(D3DMATERIAL9* pMaterial)
	{
		return m_orig->GetMaterial(pMaterial);
	}

	HRESULT __stdcall D3D9Device::SetLight(DWORD Index, CONST D3DLIGHT9* pLight)
	{
		return m_orig->SetLight(Index, pLight);
	}

	HRESULT __stdcall D3D9Device::GetLight(DWORD Index, D3DLIGHT9* pLight)
	{
		return m_orig->GetLight(Index, pLight);
	}

	HRESULT __stdcall D3D9Device::LightEnable(DWORD Index, BOOL Enable)
	{
		return m_orig->LightEnable(Index, Enable);
	}

	HRESULT __stdcall D3D9Device::GetLightEnable(DWORD Index, BOOL* pEnable)
	{
		return m_orig->GetLightEnable(Index, pEnable);
	}

	HRESULT __stdcall D3D9Device::SetClipPlane(DWORD Index, CONST float* pPlane)
	{
		return m_orig->SetClipPlane(Index, pPlane);
	}

	HRESULT __stdcall D3D9Device::GetClipPlane(DWORD Index, float* pPlane)
	{
		return m_orig->GetClipPlane(Index, pPlane);
	}

	HRESULT __stdcall D3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
	{
		return m_orig->SetRenderState(State, Value);
	}

	HRESULT __stdcall D3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
	{
		return m_orig->GetRenderState(State, pValue);
	}

	HRESULT __stdcall D3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB)
	{
		return m_orig->CreateStateBlock(Type, ppSB);
	}

	HRESULT __stdcall D3D9Device::BeginStateBlock()
	{
		return m_orig->BeginStateBlock();
	}

	HRESULT __stdcall D3D9Device::EndStateBlock(IDirect3DStateBlock9** ppSB)
	{
		return m_orig->EndStateBlock(ppSB);
	}

	HRESULT __stdcall D3D9Device::SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus)
	{
		return m_orig->SetClipStatus(pClipStatus);
	}

	HRESULT __stdcall D3D9Device::GetClipStatus(D3DCLIPSTATUS9* pClipStatus)
	{
		return m_orig->GetClipStatus(pClipStatus);
	}

	HRESULT __stdcall D3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
	{
		return m_orig->GetTexture(Stage, ppTexture);
	}

	HRESULT __stdcall D3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture)
	{
		return m_orig->SetTexture(Stage, pTexture);
	}

	HRESULT __stdcall D3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
	{
		return m_orig->GetTextureStageState(Stage, Type, pValue);
	}

	HRESULT __stdcall D3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
	{
		return m_orig->SetTextureStageState(Stage, Type, Value);
	}

	HRESULT __stdcall D3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
	{
		return m_orig->GetSamplerState(Sampler, Type, pValue);
	}

	HRESULT __stdcall D3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
	{
		return m_orig->SetSamplerState(Sampler, Type, Value);
	}

	HRESULT __stdcall D3D9Device::ValidateDevice(DWORD* pNumPasses)
	{
		return m_orig->ValidateDevice(pNumPasses);
	}

	HRESULT __stdcall D3D9Device::SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries)
	{
		return m_orig->SetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT __stdcall D3D9Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries)
	{
		return m_orig->GetPaletteEntries(PaletteNumber, pEntries);
	}

	HRESULT __stdcall D3D9Device::SetCurrentTexturePalette(UINT PaletteNumber)
	{
		return m_orig->SetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT __stdcall D3D9Device::GetCurrentTexturePalette(UINT *PaletteNumber)
	{
		return m_orig->GetCurrentTexturePalette(PaletteNumber);
	}

	HRESULT __stdcall D3D9Device::SetScissorRect(CONST RECT* pRect)
	{
		return m_orig->SetScissorRect(pRect);
	}

	HRESULT __stdcall D3D9Device::GetScissorRect(RECT* pRect)
	{
		return m_orig->GetScissorRect(pRect);
	}

	HRESULT __stdcall D3D9Device::SetSoftwareVertexProcessing(BOOL bSoftware)
	{
		return m_orig->SetSoftwareVertexProcessing(bSoftware);
	}

	BOOL __stdcall D3D9Device::GetSoftwareVertexProcessing()
	{
		return m_orig->GetSoftwareVertexProcessing();
	}

	HRESULT __stdcall D3D9Device::SetNPatchMode(float nSegments)
	{
		return m_orig->SetNPatchMode(nSegments);
	}

	float __stdcall D3D9Device::GetNPatchMode()
	{
		return m_orig->GetNPatchMode();
	}

	HRESULT __stdcall D3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
	{
		const HRESULT hr = m_orig->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
		mirror::on_after_draw(m_orig);
		return hr;
	}

	HRESULT __stdcall D3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
	{
		const HRESULT hr = m_orig->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
		mirror::on_after_draw(m_orig);
		return hr;
	}

	HRESULT __stdcall D3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
	{
		return m_orig->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
	}

	HRESULT __stdcall D3D9Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
	{
		return m_orig->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
	}

	HRESULT __stdcall D3D9Device::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags)
	{
		return m_orig->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
	}

	HRESULT __stdcall D3D9Device::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl)
	{
		return m_orig->CreateVertexDeclaration(pVertexElements, ppDecl);
	}

	HRESULT __stdcall D3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl)
	{
		return m_orig->SetVertexDeclaration(pDecl);
	}

	HRESULT __stdcall D3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl)
	{
		return m_orig->GetVertexDeclaration(ppDecl);
	}

	HRESULT __stdcall D3D9Device::SetFVF(DWORD FVF)
	{
		return m_orig->SetFVF(FVF);
	}

	HRESULT __stdcall D3D9Device::GetFVF(DWORD* pFVF)
	{
		return m_orig->GetFVF(pFVF);
	}

	HRESULT __stdcall D3D9Device::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader)
	{
		return m_orig->CreateVertexShader(pFunction, ppShader);
	}

	HRESULT __stdcall D3D9Device::SetVertexShader(IDirect3DVertexShader9* pShader)
	{
		return m_orig->SetVertexShader(pShader);
	}

	HRESULT __stdcall D3D9Device::GetVertexShader(IDirect3DVertexShader9** ppShader)
	{
		return m_orig->GetVertexShader(ppShader);
	}

	HRESULT __stdcall D3D9Device::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		mirror::on_set_vertex_shader_constant_f(m_orig, StartRegister, pConstantData, Vector4fCount);
		return m_orig->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT __stdcall D3D9Device::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		return m_orig->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT __stdcall D3D9Device::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		return m_orig->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT __stdcall D3D9Device::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		return m_orig->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT __stdcall D3D9Device::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		return m_orig->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT __stdcall D3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		return m_orig->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT __stdcall D3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride)
	{
		return m_orig->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
	}

	HRESULT __stdcall D3D9Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* OffsetInBytes, UINT* pStride)
	{
		return m_orig->GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride);
	}

	HRESULT __stdcall D3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Divider)
	{
		return m_orig->SetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT __stdcall D3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider)
	{
		return m_orig->GetStreamSourceFreq(StreamNumber, Divider);
	}

	HRESULT __stdcall D3D9Device::SetIndices(IDirect3DIndexBuffer9* pIndexData)
	{
		return m_orig->SetIndices(pIndexData);
	}

	HRESULT __stdcall D3D9Device::GetIndices(IDirect3DIndexBuffer9** ppIndexData)
	{
		return m_orig->GetIndices(ppIndexData);
	}

	HRESULT __stdcall D3D9Device::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
	{
		return m_orig->CreatePixelShader(pFunction, ppShader);
	}

	HRESULT __stdcall D3D9Device::SetPixelShader(IDirect3DPixelShader9* pShader)
	{
		return m_orig->SetPixelShader(pShader);
	}

	HRESULT __stdcall D3D9Device::GetPixelShader(IDirect3DPixelShader9** ppShader)
	{
		return m_orig->GetPixelShader(ppShader);
	}

	HRESULT __stdcall D3D9Device::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
	{
		mirror::on_set_pixel_shader_constant_f(m_orig, StartRegister, pConstantData, Vector4fCount);
		return m_orig->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT __stdcall D3D9Device::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
	{
		return m_orig->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}

	HRESULT __stdcall D3D9Device::SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
	{
		return m_orig->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT __stdcall D3D9Device::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
	{
		return m_orig->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}

	HRESULT __stdcall D3D9Device::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT  BoolCount)
	{
		return m_orig->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT __stdcall D3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
	{
		return m_orig->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}

	HRESULT __stdcall D3D9Device::DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo)
	{
		return m_orig->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
	}

	HRESULT __stdcall D3D9Device::DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo)
	{
		return m_orig->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
	}

	HRESULT __stdcall D3D9Device::DeletePatch(UINT Handle)
	{
		return m_orig->DeletePatch(Handle);
	}

	HRESULT __stdcall D3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
	{
		return m_orig->CreateQuery(Type, ppQuery);
	}

	HRESULT __stdcall D3D9Device::QueryInterface(REFIID riid, void** ppvObj)
	{
		if (ppvObj == nullptr) return E_POINTER;
		// hand back our wrapper for the device IIDs the engine actually queries.
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9))
		{
			AddRef();
			*ppvObj = this;
			return S_OK;
		}
		return m_orig->QueryInterface(riid, ppvObj);
	}

	ULONG __stdcall D3D9Device::AddRef()
	{
		return m_orig->AddRef();
	}

	ULONG __stdcall D3D9Device::Release()
	{
		const ULONG count = m_orig->Release();
		if (count == 0) delete this;
		return count;
	}

	HRESULT __stdcall D3D9Device::GetDirect3D(IDirect3D9** ppD3D9)
	{
		// Engine receives the wrapped IDirect3D9 from CreateDevice path; for now
		// just hand back the original. If anything ever queries this we'll wrap.
		return m_orig->GetDirect3D(ppD3D9);
	}
}

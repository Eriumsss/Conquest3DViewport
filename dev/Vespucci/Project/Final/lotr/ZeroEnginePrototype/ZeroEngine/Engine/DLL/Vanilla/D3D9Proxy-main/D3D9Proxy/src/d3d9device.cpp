#include "d3d9device.h"

bool g_WireframeEnabled = false;

ProxyIDirect3DDevice9::ProxyIDirect3DDevice9(IDirect3DDevice9* pReal) : m_pReal(pReal) {}

// IUnknown
HRESULT __stdcall ProxyIDirect3DDevice9::QueryInterface(REFIID riid, void** ppvObj) { return m_pReal->QueryInterface(riid, ppvObj); }
ULONG __stdcall ProxyIDirect3DDevice9::AddRef() { return m_pReal->AddRef(); }
ULONG __stdcall ProxyIDirect3DDevice9::Release() {
    ULONG count = m_pReal->Release();
    if (count == 0) { Overlay_OnReset(); delete this; }
    return count;
}

// IDirect3DDevice9 methods
HRESULT __stdcall ProxyIDirect3DDevice9::TestCooperativeLevel() { return m_pReal->TestCooperativeLevel(); }
UINT __stdcall ProxyIDirect3DDevice9::GetAvailableTextureMem() { return m_pReal->GetAvailableTextureMem(); }
HRESULT __stdcall ProxyIDirect3DDevice9::EvictManagedResources() { return m_pReal->EvictManagedResources(); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetDirect3D(IDirect3D9** ppD3D9) { return m_pReal->GetDirect3D(ppD3D9); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetDeviceCaps(D3DCAPS9* pCaps) { return m_pReal->GetDeviceCaps(pCaps); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) { return m_pReal->GetDisplayMode(iSwapChain, pMode); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParams) { return m_pReal->GetCreationParameters(pParams); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) { return m_pReal->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }
void __stdcall ProxyIDirect3DDevice9::SetCursorPosition(int X, int Y, DWORD Flags) { m_pReal->SetCursorPosition(X, Y, Flags); }
BOOL __stdcall ProxyIDirect3DDevice9::ShowCursor(BOOL bShow) { return m_pReal->ShowCursor(bShow); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pParams, IDirect3DSwapChain9** ppChain) { return m_pReal->CreateAdditionalSwapChain(pParams, ppChain); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** ppChain) { return m_pReal->GetSwapChain(iSwapChain, ppChain); }
UINT __stdcall ProxyIDirect3DDevice9::GetNumberOfSwapChains() { return m_pReal->GetNumberOfSwapChains(); }

HRESULT __stdcall ProxyIDirect3DDevice9::Reset(D3DPRESENT_PARAMETERS* pParams) {
    Overlay_OnReset();
    return m_pReal->Reset(pParams);
}

HRESULT __stdcall ProxyIDirect3DDevice9::Present(const RECT* pSrc, const RECT* pDst, HWND hWnd, const RGNDATA* pDirtyRgn) { return m_pReal->Present(pSrc, pDst, hWnd, pDirtyRgn); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppSurface) { return m_pReal->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppSurface); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) { return m_pReal->GetRasterStatus(iSwapChain, pRasterStatus); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetDialogBoxMode(BOOL bEnableDialogs) { return m_pReal->SetDialogBoxMode(bEnableDialogs); }
void __stdcall ProxyIDirect3DDevice9::SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) { m_pReal->SetGammaRamp(iSwapChain, Flags, pRamp); }
void __stdcall ProxyIDirect3DDevice9::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) { m_pReal->GetGammaRamp(iSwapChain, pRamp); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateTexture(UINT W, UINT H, UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DTexture9** ppTex, HANDLE* pHandle) { return m_pReal->CreateTexture(W, H, Levels, Usage, Fmt, Pool, ppTex, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateVolumeTexture(UINT W, UINT H, UINT D, UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DVolumeTexture9** ppTex, HANDLE* pHandle) { return m_pReal->CreateVolumeTexture(W, H, D, Levels, Usage, Fmt, Pool, ppTex, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateCubeTexture(UINT EdgeLen, UINT Levels, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DCubeTexture9** ppTex, HANDLE* pHandle) { return m_pReal->CreateCubeTexture(EdgeLen, Levels, Usage, Fmt, Pool, ppTex, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateVertexBuffer(UINT Len, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVB, HANDLE* pHandle) { return m_pReal->CreateVertexBuffer(Len, Usage, FVF, Pool, ppVB, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateIndexBuffer(UINT Len, DWORD Usage, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIB, HANDLE* pHandle) { return m_pReal->CreateIndexBuffer(Len, Usage, Fmt, Pool, ppIB, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateRenderTarget(UINT W, UINT H, D3DFORMAT Fmt, D3DMULTISAMPLE_TYPE MS, DWORD MSQ, BOOL Lockable, IDirect3DSurface9** ppSurf, HANDLE* pHandle) { return m_pReal->CreateRenderTarget(W, H, Fmt, MS, MSQ, Lockable, ppSurf, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateDepthStencilSurface(UINT W, UINT H, D3DFORMAT Fmt, D3DMULTISAMPLE_TYPE MS, DWORD MSQ, BOOL Discard, IDirect3DSurface9** ppSurf, HANDLE* pHandle) { return m_pReal->CreateDepthStencilSurface(W, H, Fmt, MS, MSQ, Discard, ppSurf, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::UpdateSurface(IDirect3DSurface9* pSrc, const RECT* pSrcRect, IDirect3DSurface9* pDst, const POINT* pDstPt) { return m_pReal->UpdateSurface(pSrc, pSrcRect, pDst, pDstPt); }
HRESULT __stdcall ProxyIDirect3DDevice9::UpdateTexture(IDirect3DBaseTexture9* pSrc, IDirect3DBaseTexture9* pDst) { return m_pReal->UpdateTexture(pSrc, pDst); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetRenderTargetData(IDirect3DSurface9* pRT, IDirect3DSurface9* pDst) { return m_pReal->GetRenderTargetData(pRT, pDst); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDst) { return m_pReal->GetFrontBufferData(iSwapChain, pDst); }
HRESULT __stdcall ProxyIDirect3DDevice9::StretchRect(IDirect3DSurface9* pSrc, const RECT* pSrcRect, IDirect3DSurface9* pDst, const RECT* pDstRect, D3DTEXTUREFILTERTYPE Filter) { return m_pReal->StretchRect(pSrc, pSrcRect, pDst, pDstRect, Filter); }
HRESULT __stdcall ProxyIDirect3DDevice9::ColorFill(IDirect3DSurface9* pSurf, const RECT* pRect, D3DCOLOR Color) { return m_pReal->ColorFill(pSurf, pRect, Color); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateOffscreenPlainSurface(UINT W, UINT H, D3DFORMAT Fmt, D3DPOOL Pool, IDirect3DSurface9** ppSurf, HANDLE* pHandle) { return m_pReal->CreateOffscreenPlainSurface(W, H, Fmt, Pool, ppSurf, pHandle); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetRenderTarget(DWORD RTIndex, IDirect3DSurface9* pRT) { return m_pReal->SetRenderTarget(RTIndex, pRT); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetRenderTarget(DWORD RTIndex, IDirect3DSurface9** ppRT) { return m_pReal->GetRenderTarget(RTIndex, ppRT); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) { return m_pReal->SetDepthStencilSurface(pNewZStencil); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetDepthStencilSurface(IDirect3DSurface9** ppZStencil) { return m_pReal->GetDepthStencilSurface(ppZStencil); }

// BeginScene - set wireframe here!
HRESULT __stdcall ProxyIDirect3DDevice9::BeginScene() {
    HRESULT hr = m_pReal->BeginScene();
    Overlay_OnBeginScene(m_pReal);
    if (g_WireframeEnabled) {
        m_pReal->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
    }
    return hr;
}

// EndScene - draw overlay here!
HRESULT __stdcall ProxyIDirect3DDevice9::EndScene() {
    m_pReal->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    Overlay_OnEndScene(m_pReal);
    return m_pReal->EndScene();
}

HRESULT __stdcall ProxyIDirect3DDevice9::Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) { return m_pReal->Clear(Count, pRects, Flags, Color, Z, Stencil); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) { return m_pReal->SetTransform(State, pMatrix); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) { return m_pReal->GetTransform(State, pMatrix); }
HRESULT __stdcall ProxyIDirect3DDevice9::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) { return m_pReal->MultiplyTransform(State, pMatrix); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetViewport(const D3DVIEWPORT9* pViewport) { return m_pReal->SetViewport(pViewport); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetViewport(D3DVIEWPORT9* pViewport) { return m_pReal->GetViewport(pViewport); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetMaterial(const D3DMATERIAL9* pMaterial) { return m_pReal->SetMaterial(pMaterial); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetMaterial(D3DMATERIAL9* pMaterial) { return m_pReal->GetMaterial(pMaterial); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetLight(DWORD Index, const D3DLIGHT9* pLight) { return m_pReal->SetLight(Index, pLight); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetLight(DWORD Index, D3DLIGHT9* pLight) { return m_pReal->GetLight(Index, pLight); }
HRESULT __stdcall ProxyIDirect3DDevice9::LightEnable(DWORD Index, BOOL Enable) { return m_pReal->LightEnable(Index, Enable); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetLightEnable(DWORD Index, BOOL* pEnable) { return m_pReal->GetLightEnable(Index, pEnable); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetClipPlane(DWORD Index, const float* pPlane) { return m_pReal->SetClipPlane(Index, pPlane); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetClipPlane(DWORD Index, float* pPlane) { return m_pReal->GetClipPlane(Index, pPlane); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) { return m_pReal->SetRenderState(State, Value); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) { return m_pReal->GetRenderState(State, pValue); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) { return m_pReal->CreateStateBlock(Type, ppSB); }
HRESULT __stdcall ProxyIDirect3DDevice9::BeginStateBlock() { return m_pReal->BeginStateBlock(); }
HRESULT __stdcall ProxyIDirect3DDevice9::EndStateBlock(IDirect3DStateBlock9** ppSB) { return m_pReal->EndStateBlock(ppSB); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) { return m_pReal->SetClipStatus(pClipStatus); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) { return m_pReal->GetClipStatus(pClipStatus); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) { return m_pReal->GetTexture(Stage, ppTexture); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) { return m_pReal->SetTexture(Stage, pTexture); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) { return m_pReal->GetTextureStageState(Stage, Type, pValue); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) { return m_pReal->SetTextureStageState(Stage, Type, Value); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) { return m_pReal->GetSamplerState(Sampler, Type, pValue); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) { return m_pReal->SetSamplerState(Sampler, Type, Value); }
HRESULT __stdcall ProxyIDirect3DDevice9::ValidateDevice(DWORD* pNumPasses) { return m_pReal->ValidateDevice(pNumPasses); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) { return m_pReal->SetPaletteEntries(PaletteNumber, pEntries); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) { return m_pReal->GetPaletteEntries(PaletteNumber, pEntries); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetCurrentTexturePalette(UINT PaletteNumber) { return m_pReal->SetCurrentTexturePalette(PaletteNumber); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetCurrentTexturePalette(UINT* PaletteNumber) { return m_pReal->GetCurrentTexturePalette(PaletteNumber); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetScissorRect(const RECT* pRect) { return m_pReal->SetScissorRect(pRect); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetScissorRect(RECT* pRect) { return m_pReal->GetScissorRect(pRect); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetSoftwareVertexProcessing(BOOL bSoftware) { return m_pReal->SetSoftwareVertexProcessing(bSoftware); }
BOOL __stdcall ProxyIDirect3DDevice9::GetSoftwareVertexProcessing() { return m_pReal->GetSoftwareVertexProcessing(); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetNPatchMode(float nSegments) { return m_pReal->SetNPatchMode(nSegments); }
float __stdcall ProxyIDirect3DDevice9::GetNPatchMode() { return m_pReal->GetNPatchMode(); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawPrimitive(D3DPRIMITIVETYPE Type, UINT StartVertex, UINT PrimitiveCount) { return m_pReal->DrawPrimitive(Type, StartVertex, PrimitiveCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount) { return m_pReal->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawPrimitiveUP(D3DPRIMITIVETYPE Type, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { return m_pReal->DrawPrimitiveUP(Type, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE Type, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) { return m_pReal->DrawIndexedPrimitiveUP(Type, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }
HRESULT __stdcall ProxyIDirect3DDevice9::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) { return m_pReal->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) { return m_pReal->CreateVertexDeclaration(pVertexElements, ppDecl); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) { return m_pReal->SetVertexDeclaration(pDecl); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) { return m_pReal->GetVertexDeclaration(ppDecl); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetFVF(DWORD FVF) { return m_pReal->SetFVF(FVF); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetFVF(DWORD* pFVF) { return m_pReal->GetFVF(pFVF); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) { return m_pReal->CreateVertexShader(pFunction, ppShader); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexShader(IDirect3DVertexShader9* pShader) { return m_pReal->SetVertexShader(pShader); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexShader(IDirect3DVertexShader9** ppShader) { return m_pReal->GetVertexShader(ppShader); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) { return m_pReal->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return m_pReal->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) { return m_pReal->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return m_pReal->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetVertexShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) { return m_pReal->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) { return m_pReal->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) { return m_pReal->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) { return m_pReal->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) { return m_pReal->SetStreamSourceFreq(StreamNumber, Setting); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) { return m_pReal->GetStreamSourceFreq(StreamNumber, pSetting); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetIndices(IDirect3DIndexBuffer9* pIndexData) { return m_pReal->SetIndices(pIndexData); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetIndices(IDirect3DIndexBuffer9** ppIndexData) { return m_pReal->GetIndices(ppIndexData); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) { return m_pReal->CreatePixelShader(pFunction, ppShader); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetPixelShader(IDirect3DPixelShader9* pShader) { return m_pReal->SetPixelShader(pShader); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetPixelShader(IDirect3DPixelShader9** ppShader) { return m_pReal->GetPixelShader(ppShader); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) { return m_pReal->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) { return m_pReal->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) { return m_pReal->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) { return m_pReal->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) { return m_pReal->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) { return m_pReal->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawRectPatch(UINT Handle, const float* pNumSegs, const D3DRECTPATCH_INFO* pRectPatchInfo) { return m_pReal->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
HRESULT __stdcall ProxyIDirect3DDevice9::DrawTriPatch(UINT Handle, const float* pNumSegs, const D3DTRIPATCH_INFO* pTriPatchInfo) { return m_pReal->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
HRESULT __stdcall ProxyIDirect3DDevice9::DeletePatch(UINT Handle) { return m_pReal->DeletePatch(Handle); }
HRESULT __stdcall ProxyIDirect3DDevice9::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) { return m_pReal->CreateQuery(Type, ppQuery); }


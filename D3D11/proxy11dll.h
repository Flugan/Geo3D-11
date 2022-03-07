// proxydll.h
#pragma once
#include "stdafx.h"

// regular functions
typedef void(STDMETHODCALLTYPE* D3D11_GIC)(ID3D11Device* This, ID3D11DeviceContext** ppImmediateContext);
static struct {
	SIZE_T nHookId;
	D3D11_GIC fn;
} sGetImmediateContext_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_VS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11VertexShader **ppVertexShader);
static struct {
	SIZE_T nHookId;
	D3D11_VS fnCreateVertexShader;
} sCreateVertexShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_PS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11PixelShader **ppPixelShader);
static struct {
	SIZE_T nHookId;
	D3D11_PS fnCreatePixelShader;
} sCreatePixelShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_GS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11GeometryShader **ppGeometryShader);
static struct {
	SIZE_T nHookId;
	D3D11_GS fnCreateGeometryShader;
} sCreateGeometryShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_HS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11HullShader **ppHullShader);
static struct {
	SIZE_T nHookId;
	D3D11_HS fnCreateHullShader;
} sCreateHullShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_DS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11DomainShader **ppDomainShader);
static struct {
	SIZE_T nHookId;
	D3D11_DS fnCreateDomainShader;
} sCreateDomainShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_CS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11ComputeShader **ppComputeShader);
static struct {
	SIZE_T nHookId;
	D3D11_CS fnCreateComputeShader;
} sCreateComputeShader_Hook = { 0, NULL };
typedef HRESULT(STDMETHODCALLTYPE* D3D11_2D)(
	ID3D11Device * This,
	_In_ const D3D11_TEXTURE2D_DESC *pDesc,
	_In_opt_ const D3D11_SUBRESOURCE_DATA *pInitialData,
	_Out_opt_ ID3D11Texture2D **ppTexture2D);
static struct {
	SIZE_T nHookId;
	D3D11_2D fnCreateTexture2D;
} sCreateTexture2D_Hook = { 0, NULL };
typedef HRESULT(STDMETHODCALLTYPE* D3D11_3D)(
	ID3D11Device * This,
	_In_ const D3D11_TEXTURE3D_DESC *pDesc,
	_In_opt_ const D3D11_SUBRESOURCE_DATA *pInitialData,
	_Out_opt_ ID3D11Texture3D **ppTexture2D);
static struct {
	SIZE_T nHookId;
	D3D11_3D fnCreateTexture3D;
} sCreateTexture3D_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_Draw)(ID3D11DeviceContext * This, UINT VertexCount, UINT StartVertexLocation);
static struct {
	SIZE_T nHookId;
	D3D11C_Draw fnDraw;
} sDraw_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_DrawAuto)(ID3D11DeviceContext * This);
static struct {
	SIZE_T nHookId;
	D3D11C_DrawAuto fnDrawAuto;
} sDrawAuto_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_DrawIndexed)(ID3D11DeviceContext * This, UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation);
static struct {
	SIZE_T nHookId;
	D3D11C_DrawIndexed fnDrawIndexed;
} sDrawIndexed_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_DrawInstanced)(ID3D11DeviceContext * This, UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation);
static struct {
	SIZE_T nHookId;
	D3D11C_DrawInstanced fnDrawInstanced;
} sDrawInstanced_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_DrawIndexedInstanced)(ID3D11DeviceContext * This, UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation);
static struct {
	SIZE_T nHookId;
	D3D11C_DrawIndexedInstanced fnDrawIndexedInstanced;
} sDrawIndexedInstanced_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_PSSS)(ID3D11DeviceContext * This, ID3D11PixelShader *pPixelShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_PSSS fnPSSetShader;
} sPSSetShader_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_VSSS)(ID3D11DeviceContext * This, ID3D11VertexShader *pVertexShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_VSSS fnVSSetShader;
} sVSSetShader_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_CSSS)(ID3D11DeviceContext * This, ID3D11ComputeShader *pComputeShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_CSSS fnCSSetShader;
} sCSSetShader_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_GSSS)(ID3D11DeviceContext * This, ID3D11GeometryShader *pComputeShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_GSSS fnGSSetShader;
} sGSSetShader_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_HSSS)(ID3D11DeviceContext * This, ID3D11HullShader *pHullShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_HSSS fnHSSetShader;
} sHSSetShader_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_DSSS)(ID3D11DeviceContext * This, ID3D11DomainShader *pDomainShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_DSSS fnDSSetShader;
} sDSSetShader_Hook = { 0, NULL };
typedef HRESULT(STDMETHODCALLTYPE* DXGI_Present)(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
static struct {
	SIZE_T nHookId;
	DXGI_Present fnDXGI_Present;
} sDXGI_Present_Hook = { 0, NULL };
typedef HRESULT(STDMETHODCALLTYPE* DXGI_ResizeBuffers)(IDXGISwapChain* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
static struct {
	SIZE_T nHookId;
	DXGI_ResizeBuffers fnDXGI_ResizeBuffers;
} sDXGI_ResizeBuffers_Hook = { 0, NULL };
typedef HRESULT(STDMETHODCALLTYPE* DXGI_CSC1)(IDXGIFactory1 * This, IUnknown * pDevice, DXGI_SWAP_CHAIN_DESC * pDesc, IDXGISwapChain ** ppSwapChain);
static struct {
	SIZE_T nHookId;
	DXGI_CSC1 fnCreateSwapChain1;
} sCreateSwapChain_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_CSR)(ID3D11DeviceContext * This, ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource, UINT SrcSubresource, const D3D11_BOX *pSrcBox);
static struct {
	SIZE_T nHookId;
	D3D11C_CSR fnCopySubresourceRegion;
} sCopySubresourceRegion_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_UNMAP)(ID3D11DeviceContext * This, ID3D11Resource *pResource, UINT Subresource);
static struct {
	SIZE_T nHookId;
	D3D11C_UNMAP fnUnmap;
} sUnmap_Hook = { 0, NULL };
typedef HRESULT(STDMETHODCALLTYPE* D3D11C_MAP)(ID3D11DeviceContext * This, ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource);
static struct {
	SIZE_T nHookId;
	D3D11C_MAP fnMap;
} sMap_Hook = { 0, NULL };

void InitInstance();
void ExitInstance();
void LoadOriginalDll();
void hook(ID3D11DeviceContext**);
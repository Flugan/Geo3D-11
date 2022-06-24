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
	D3D11_VS fn;
} sCreateVertexShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_PS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11PixelShader **ppPixelShader);
static struct {
	SIZE_T nHookId;
	D3D11_PS fn;
} sCreatePixelShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_GS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11GeometryShader **ppGeometryShader);
static struct {
	SIZE_T nHookId;
	D3D11_GS fn;
} sCreateGeometryShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_HS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11HullShader **ppHullShader);
static struct {
	SIZE_T nHookId;
	D3D11_HS fn;
} sCreateHullShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_DS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11DomainShader **ppDomainShader);
static struct {
	SIZE_T nHookId;
	D3D11_DS fn;
} sCreateDomainShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* D3D11_CS)(
	ID3D11Device * This,
	_In_ const void *pShaderBytecode,
	_In_ SIZE_T BytecodeLength,
	_In_opt_ ID3D11ClassLinkage *pClassLinkage,
	_Out_opt_ ID3D11ComputeShader **ppComputeShader);
static struct {
	SIZE_T nHookId;
	D3D11_CS fn;
} sCreateComputeShader_Hook = { 0, NULL };

typedef void(STDMETHODCALLTYPE* D3D11C_PSSS)(ID3D11DeviceContext * This, ID3D11PixelShader *pPixelShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_PSSS fn;
} sPSSetShader_Hook = { 0, NULL };

typedef void(STDMETHODCALLTYPE* D3D11C_VSSS)(ID3D11DeviceContext * This, ID3D11VertexShader *pVertexShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_VSSS fn;
} sVSSetShader_Hook = { 0, NULL };
typedef void(STDMETHODCALLTYPE* D3D11C_CSSS)(ID3D11DeviceContext * This, ID3D11ComputeShader *pComputeShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_CSSS fn;
} sCSSetShader_Hook = { 0, NULL };

typedef void(STDMETHODCALLTYPE* D3D11C_GSSS)(ID3D11DeviceContext * This, ID3D11GeometryShader *pComputeShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_GSSS fn;
} sGSSetShader_Hook = { 0, NULL };

typedef void(STDMETHODCALLTYPE* D3D11C_HSSS)(ID3D11DeviceContext * This, ID3D11HullShader *pHullShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_HSSS fn;
} sHSSetShader_Hook = { 0, NULL };

typedef void(STDMETHODCALLTYPE* D3D11C_DSSS)(ID3D11DeviceContext * This, ID3D11DomainShader *pDomainShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances);
static struct {
	SIZE_T nHookId;
	D3D11C_DSSS fn;
} sDSSetShader_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* DXGI_Present)(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
static struct {
	SIZE_T nHookId;
	DXGI_Present fn;
} sDXGI_Present_Hook = { 0, NULL };

typedef HRESULT(STDMETHODCALLTYPE* DXGI_CSC)(IDXGIFactory1 * This, IUnknown * pDevice, DXGI_SWAP_CHAIN_DESC * pDesc, IDXGISwapChain ** ppSwapChain);
static struct {
	SIZE_T nHookId;
	DXGI_CSC fn;
} sCreateSwapChain_Hook = { 0, NULL };

void InitInstance();
void ExitInstance();
void LoadOriginalDll();
void hook(ID3D11DeviceContext**);
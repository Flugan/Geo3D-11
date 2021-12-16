// proxydll.cpp
#include "proxy11dll.h"
#define NO_STEREO_D3D9
#define NO_STEREO_D3D10
#include "nvstereo.h"
#include "Nektra\NktHookLib.h"
#include "log.h"
#include <map>
#include <DirectXMath.h>
#include <D3Dcompiler.h>
#include "vkeys.h"
#include <algorithm>
#include <Xinput.h>
#include "resource.h"
#define _USE_MATH_DEFINES
#include <math.h>

// global variables
#pragma data_seg (".d3d11_shared")
HINSTANCE           gl_hOriginalDll;
HINSTANCE           gl_hThisInstance;
bool				gl_hookedDevice = false;
bool				gl_hookedContext = false;
bool				gl_Present_hooked = false;
bool				gl_dump = false;
bool				gl_log = false;
bool				gl_cache_shaders = false;
bool				gl_hunt = false;
bool				gl_nvapi = false;
char				cwd[MAX_PATH];
FILE *LogFile = 0;		// off by default.
bool gLogDebug = false;
CRITICAL_SECTION	gl_CS;
DirectX::XMFLOAT4	iniParams[INI_PARAMS_SIZE];
nv::stereo::ParamTextureManagerD3D11 *gStereoTexMgr = NULL;
ID3D11DeviceContext * gContext = NULL;
ID3D11Device *gDevice = NULL;
StereoHandle gStereoHandle = NULL;
ID3D11Texture2D *gStereoTexture = NULL;
ID3D11ShaderResourceView *gStereoResourceView = NULL;
ID3D11Texture1D *gIniTexture = NULL;
ID3D11ShaderResourceView *gIniResourceView = NULL;
ResolutionInfo gResolutionInfo;

map<UINT64, bool> isCache;
map<UINT64, bool> hasStartPatch;
map<UINT64, bool> hasStartFix;

// Used for deny_cpu_read texture override
typedef std::unordered_map<ID3D11Resource *, void *> DeniedMap;
DeniedMap mDeniedMaps;
#pragma data_seg ()
// The Log file and the Globals are both used globally, and these are the actual
// definitions of the variables.  All other uses will be via the extern in the 
// globals.h and log.h files.



CNktHookLib cHookMgr;

#pragma region hook
typedef HMODULE(WINAPI *lpfnLoadLibraryExW)(_In_ LPCWSTR lpLibFileName, _Reserved_ HANDLE hFile, _In_ DWORD dwFlags);
static HMODULE WINAPI Hooked_LoadLibraryExW(_In_ LPCWSTR lpLibFileName, _Reserved_ HANDLE hFile, _In_ DWORD dwFlags);
static struct
{
	SIZE_T nHookId;
	lpfnLoadLibraryExW fnLoadLibraryExW;
} sLoadLibraryExW_Hook = { 0, NULL };

// ----------------------------------------------------------------------------

static HMODULE ReplaceOnMatch(LPCWSTR lpLibFileName, HANDLE hFile,
	DWORD dwFlags, LPCWSTR our_name, LPCWSTR library)
{
	WCHAR fullPath[MAX_PATH];

	// We can use System32 for all cases, because it will be properly rerouted
	// to SysWow64 by LoadLibraryEx itself.

	if (GetSystemDirectoryW(fullPath, ARRAYSIZE(fullPath)) == 0)
		return NULL;
	wcscat_s(fullPath, MAX_PATH, L"\\");
	wcscat_s(fullPath, MAX_PATH, library);

	// Bypass the known expected call from our wrapped d3d11 & nvapi, where it needs
	// to call to the system to get APIs. This is a bit of a hack, but if the string
	// comes in as original_d3d11/nvapi/nvapi64, that's from us, and needs to switch 
	// to the real one. The test string should have no path attached.

	if (_wcsicmp(lpLibFileName, our_name) == 0)
	{
		//LogInfoW(L"Hooked_LoadLibraryExW switching to original dll: %s to %s.\n",
			//lpLibFileName, fullPath);

		return sLoadLibraryExW_Hook.fnLoadLibraryExW(fullPath, hFile, dwFlags);
	}

	// For this case, we want to see if it's the game loading d3d11 or nvapi directly
	// from the system directory, and redirect it to the game folder if so, by stripping
	// the system path. This is to be case insensitive as we don't know if NVidia will 
	// change that and otherwise break it it with a driver upgrade. 

	if (_wcsicmp(lpLibFileName, fullPath) == 0)
	{
		//LogInfoW(L"Replaced Hooked_LoadLibraryExW for: %s to %s.\n", lpLibFileName, library);

		return sLoadLibraryExW_Hook.fnLoadLibraryExW(library, hFile, dwFlags);
	}

	return NULL;
}

// Function called for every LoadLibraryExW call once we have hooked it.
// We want to look for overrides to System32 that we can circumvent.  This only happens
// in the current process, not system wide.
// 
// We need to do two things here.  First, we need to bypass all calls that go
// directly to the System32 folder, because that will circumvent our wrapping 
// of the d3d11 and nvapi APIs. The nvapi itself does this specifically as fake
// security to avoid proxy DLLs like us. 
// Second, because we are now forcing all LoadLibraryExW calls back to the game
// folder, we need somehow to allow us access to the original dlls so that we can
// get the original proc addresses to call.  We do this with the original_* names
// passed in to this routine.
//
// There three use cases:
// x32 game on x32 OS
//	 LoadLibraryExW("C:\Windows\system32\d3d11.dll", NULL, 0)
//	 LoadLibraryExW("C:\Windows\system32\nvapi.dll", NULL, 0)
// x64 game on x64 OS
//	 LoadLibraryExW("C:\Windows\system32\d3d11.dll", NULL, 0)
//	 LoadLibraryExW("C:\Windows\system32\nvapi64.dll", NULL, 0)
// x32 game on x64 OS
//	 LoadLibraryExW("C:\Windows\SysWOW64\d3d11.dll", NULL, 0)
//	 LoadLibraryExW("C:\Windows\SysWOW64\nvapi.dll", NULL, 0)
//
// To be general and simplify the init, we are going to specifically do the bypass 
// for all variants, even though we only know of this happening on x64 games.  
//
// An important thing to remember here is that System32 is automatically rerouted
// to SysWow64 by the OS as necessary, so we can use System32 in all cases.
//
// It's not clear if we should also hook LoadLibraryW, but we don't have examples
// where we need that yet.

static HMODULE WINAPI Hooked_LoadLibraryExW(_In_ LPCWSTR lpLibFileName, _Reserved_ HANDLE hFile, _In_ DWORD dwFlags)
{
	HMODULE module;

	module = ReplaceOnMatch(lpLibFileName, hFile, dwFlags, L"original_d3d11.dll", L"d3d11.dll");
	if (module)
		return module;

	module = ReplaceOnMatch(lpLibFileName, hFile, dwFlags, L"original_nvapi64.dll", L"nvapi64.dll");
	if (module) {
		gl_nvapi = true;
		return module;
	}

	module = ReplaceOnMatch(lpLibFileName, hFile, dwFlags, L"original_nvapi.dll", L"nvapi.dll");
	if (module) {
		gl_nvapi = true;
		return module;
	}

	// Normal unchanged case.
	return sLoadLibraryExW_Hook.fnLoadLibraryExW(lpLibFileName, hFile, dwFlags);
}

static bool InstallHooks()
{
	HINSTANCE hKernel32;
	LPVOID fnOrigLoadLibrary;
	DWORD dwOsErr;

	hKernel32 = NktHookLibHelpers::GetModuleBaseAddress(L"Kernel32.dll");
	if (hKernel32 == NULL)
		return false;

	// Only ExW version for now, used by nvapi.
	fnOrigLoadLibrary = NktHookLibHelpers::GetProcedureAddress(hKernel32, "LoadLibraryExW");
	if (fnOrigLoadLibrary == NULL)
		return false;

	dwOsErr = cHookMgr.Hook(&(sLoadLibraryExW_Hook.nHookId), (LPVOID*)&(sLoadLibraryExW_Hook.fnLoadLibraryExW),
		fnOrigLoadLibrary, Hooked_LoadLibraryExW);

	return (dwOsErr == 0) ? true : false;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	bool result = true;

	switch (fdwReason) {
	case DLL_PROCESS_ATTACH:
		gl_hThisInstance = hinstDLL;
		ShowStartupScreen();
		result = InstallHooks();
		InitInstance();
		break;

	case DLL_PROCESS_DETACH:
		ExitInstance();
		break;

	case DLL_THREAD_ATTACH:
		// Do thread-specific initialization.
		break;

	case DLL_THREAD_DETACH:
		// Do thread-specific cleanup.
		break;
	}

	return result;
}
#pragma endregion

// Primary hash calculation for all shader file names, all textures.
// 64 bit magic FNV-0 and FNV-1 prime
#define FNV_64_PRIME ((UINT64)0x100000001b3ULL)
static UINT64 fnv_64_buf(const void *buf, size_t len)
{
	UINT64 hval = 0;
	unsigned const char *bp = (unsigned const char *)buf;	/* start of buffer */
	unsigned const char *be = bp + len;		/* beyond end of buffer */

											// FNV-1 hash each octet of the buffer
	while (bp < be)
	{
		// multiply by the 64 bit FNV magic prime mod 2^64 */
		hval *= FNV_64_PRIME;
		// xor the bottom with the current octet
		hval ^= (UINT64)*bp++;
	}
	return hval;
}

#pragma region Create
void dump(const void* pShaderBytecode, SIZE_T BytecodeLength, char* buffer) {
	char path[MAX_PATH];
	path[0] = 0;
	strcat_s(path, MAX_PATH, cwd);
	strcat_s(path, MAX_PATH, "\\ShaderCache");
	CreateDirectory(path, NULL);
	strcat_s(path, MAX_PATH, "\\");
	strcat_s(path, MAX_PATH, buffer);
	strcat_s(path, MAX_PATH, ".bin");
	EnterCriticalSection(&gl_CS);
	FILE* f;
	fopen_s(&f, path, "wb");
	fwrite(pShaderBytecode, 1, BytecodeLength, f);
	fclose(f);
	LeaveCriticalSection(&gl_CS);
}
vector<byte> cached(char* buffer) {
	char path[MAX_PATH];
	path[0] = 0;
	strcat_s(path, MAX_PATH, cwd);
	strcat_s(path, MAX_PATH, "\\ShaderFixes\\");
	strcat_s(path, MAX_PATH, buffer);
	strcat_s(path, MAX_PATH, ".bin");
	auto file = readFile(path);
	return file;
}
vector<byte> assembled(char* buffer, const void* pShaderBytecode, SIZE_T BytecodeLength) {
	char path[MAX_PATH];
	path[0] = 0;
	strcat_s(path, MAX_PATH, cwd);
	strcat_s(path, MAX_PATH, "\\ShaderFixes\\");
	strcat_s(path, MAX_PATH, buffer);
	strcat_s(path, MAX_PATH, ".txt");
	auto file = readFile(path);

	vector<byte>* v = new vector<byte>(BytecodeLength);
	copy((byte*)pShaderBytecode, (byte*)pShaderBytecode + BytecodeLength, v->begin());

	vector<byte> byteCode = assembler(file, *v);
	if (gl_cache_shaders) {
		FILE* f;
		path[0] = 0;
		strcat_s(path, MAX_PATH, cwd);
		strcat_s(path, MAX_PATH, "\\ShaderFixes\\");
		strcat_s(path, MAX_PATH, buffer);
		strcat_s(path, MAX_PATH, ".bin");

		EnterCriticalSection(&gl_CS);
		fopen_s(&f, path, "wb");
		fwrite(byteCode.data(), 1, byteCode.size(), f);
		fclose(f);
		LeaveCriticalSection(&gl_CS);
	}
	return byteCode;
}
ID3DBlob* hlsled(char* buffer, char* shdModel){
	char path[MAX_PATH];
	path[0] = 0;
	strcat_s(path, MAX_PATH, cwd);
	strcat_s(path, MAX_PATH, "\\ShaderFixes\\");
	strcat_s(path, MAX_PATH, buffer);
	strcat_s(path, MAX_PATH, "_replace.txt");
	auto file = readFile(path);

	ID3DBlob* pByteCode = nullptr;
	ID3DBlob* pErrorMsgs = nullptr;
	HRESULT ret = D3DCompile(file.data(), file.size(), NULL, 0, ((ID3DInclude*)(UINT_PTR)1),
		"main", shdModel, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pByteCode, &pErrorMsgs);
	if (SUCCEEDED(ret)) {
		if (gl_cache_shaders) {
			path[0] = 0;
			strcat_s(path, MAX_PATH, cwd);
			strcat_s(path, MAX_PATH, "\\ShaderFixes\\");
			strcat_s(path, MAX_PATH, buffer);
			strcat_s(path, MAX_PATH, ".bin");

			EnterCriticalSection(&gl_CS);
			FILE* f;
			fopen_s(&f, path, "wb");
			fwrite(pByteCode->GetBufferPointer(), 1, pByteCode->GetBufferSize(), f);
			fclose(f);
			LeaveCriticalSection(&gl_CS);
		}
	}
	return pByteCode;
}

HRESULT STDMETHODCALLTYPE D3D11_CreateVertexShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create VertexShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-vs", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	ID3D11VertexShader * shader;
	HRESULT res;
	if (isCache.count(_crc)) {
		auto file = cached(buffer);
		res = sCreateVertexShader_Hook.fnCreateVertexShader(This, file.data(), file.size(), pClassLinkage, &shader);
	} else if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		res = sCreateVertexShader_Hook.fnCreateVertexShader(This, data.data(), data.size(), pClassLinkage, &shader);
	} else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "vs_5_0");
		res = sCreateVertexShader_Hook.fnCreateVertexShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, &shader);
	} else {
		res = sCreateVertexShader_Hook.fnCreateVertexShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
	}
	return res;
}

HRESULT STDMETHODCALLTYPE D3D11_CreatePixelShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create PixelShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-ps", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT res;
	if (isCache.count(_crc)) {
		auto file = cached(buffer);
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, file.data(), file.size(), pClassLinkage, ppPixelShader);
	} else if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, data.data(), data.size(), pClassLinkage, ppPixelShader);
	} else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "ps_5_0");
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppPixelShader);
	} else {
		res = sCreatePixelShader_Hook.fnCreatePixelShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader);
	}
	return res;
}

HRESULT STDMETHODCALLTYPE D3D11_CreateGeometryShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create GeometryShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-gs", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT res;
	if (isCache.count(_crc)) {
		auto file = cached(buffer);
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, file.data(), file.size(), pClassLinkage, ppGeometryShader);
	} else if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, data.data(), data.size(), pClassLinkage, ppGeometryShader);
	} else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "gs_5_0");
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppGeometryShader);
	} else {
		res = sCreateGeometryShader_Hook.fnCreateGeometryShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader);
	}
	return res;
}

HRESULT STDMETHODCALLTYPE D3D11_CreateHullShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11HullShader **ppHullShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create HullShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-hs", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT res;
	if (isCache.count(_crc)) {
		auto file = cached(buffer);
		res = sCreateHullShader_Hook.fnCreateHullShader(This, file.data(), file.size(), pClassLinkage, ppHullShader);
	} else if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		res = sCreateHullShader_Hook.fnCreateHullShader(This, data.data(), data.size(), pClassLinkage, ppHullShader);
	} else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "hs_5_0");
		res = sCreateHullShader_Hook.fnCreateHullShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppHullShader);
	} else {
		res = sCreateHullShader_Hook.fnCreateHullShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader);
	}
	return res;
}

HRESULT STDMETHODCALLTYPE D3D11_CreateDomainShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11DomainShader **ppDomainShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create DomainShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-ds", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT res;
	if (isCache.count(_crc)) {
		auto file = cached(buffer);
		res = sCreateDomainShader_Hook.fnCreateDomainShader(This, file.data(), file.size(), pClassLinkage, ppDomainShader);
	} else if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		res = sCreateDomainShader_Hook.fnCreateDomainShader(This, data.data(), data.size(), pClassLinkage, ppDomainShader);
	} else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "ds_5_0");
		res = sCreateDomainShader_Hook.fnCreateDomainShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppDomainShader);
	} else {
		res = sCreateDomainShader_Hook.fnCreateDomainShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader);
	}
	return res;
}

HRESULT STDMETHODCALLTYPE D3D11_CreateComputeShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11ComputeShader **ppComputeShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	UINT64 _crc2 = 0;
	LogInfo("Create ComputeShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-cs", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT res;
	if (isCache.count(_crc)) {
		auto file = cached(buffer);
		res = sCreateComputeShader_Hook.fnCreateComputeShader(This, file.data(), file.size(), pClassLinkage, ppComputeShader);
	} else if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		res = sCreateComputeShader_Hook.fnCreateComputeShader(This, data.data(), data.size(), pClassLinkage, ppComputeShader);
	} else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "ds_5_0");
		res = sCreateComputeShader_Hook.fnCreateComputeShader(This, pByteCode->GetBufferPointer(), pByteCode->GetBufferSize(), pClassLinkage, ppComputeShader);
	} else {
		res = sCreateComputeShader_Hook.fnCreateComputeShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader);
	}
	return res;
}
#pragma endregion

#pragma region SetShader
map<UINT64, ID3D11PixelShader*> RunningPS;
UINT64 currentPScrc;
void STDMETHODCALLTYPE D3D11C_PSSetShader(ID3D11DeviceContext * This, ID3D11PixelShader *pPixelShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sPSSetShader_Hook.fnPSSetShader(This, pPixelShader, ppClassInstances, NumClassInstances);
	This->PSSetShaderResources(125, 1, &gStereoResourceView);
	This->PSSetShaderResources(120, 1, &gIniResourceView);
}
map<UINT64, ID3D11VertexShader*> RunningVS;
UINT64 currentVScrc;
void STDMETHODCALLTYPE D3D11C_VSSetShader(ID3D11DeviceContext * This, ID3D11VertexShader *pVertexShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sVSSetShader_Hook.fnVSSetShader(This, pVertexShader, ppClassInstances, NumClassInstances);
	This->VSSetShaderResources(125, 1, &gStereoResourceView);
	This->VSSetShaderResources(120, 1, &gIniResourceView);
}
void STDMETHODCALLTYPE D3D11C_CSSetShader(ID3D11DeviceContext * This, ID3D11ComputeShader *pComputeShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sCSSetShader_Hook.fnCSSetShader(This, pComputeShader, ppClassInstances, NumClassInstances);
	This->CSSetShaderResources(125, 1, &gStereoResourceView);
	This->CSSetShaderResources(120, 1, &gIniResourceView);
}
void STDMETHODCALLTYPE D3D11C_GSSetShader(ID3D11DeviceContext * This, ID3D11GeometryShader *pGeometryShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sGSSetShader_Hook.fnGSSetShader(This, pGeometryShader, ppClassInstances, NumClassInstances);
	This->GSSetShaderResources(125, 1, &gStereoResourceView);
	This->GSSetShaderResources(120, 1, &gIniResourceView);
}
void STDMETHODCALLTYPE D3D11C_HSSetShader(ID3D11DeviceContext * This, ID3D11HullShader *pHullShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sHSSetShader_Hook.fnHSSetShader(This, pHullShader, ppClassInstances, NumClassInstances);
	This->HSSetShaderResources(125, 1, &gStereoResourceView);
	This->HSSetShaderResources(120, 1, &gIniResourceView);
}
void STDMETHODCALLTYPE D3D11C_DSSetShader(ID3D11DeviceContext * This, ID3D11DomainShader *pDomainShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sDSSetShader_Hook.fnDSSetShader(This, pDomainShader, ppClassInstances, NumClassInstances);
	This->DSSetShaderResources(125, 1, &gStereoResourceView);
	This->DSSetShaderResources(120, 1, &gIniResourceView);
}
#pragma endregion

#pragma region Button
enum buttonPress { Unchanged, Down, Up };

class button {
public:
	virtual buttonPress buttonCheck() = 0;
};

class keyboardMouseKey : public button {
public:
	keyboardMouseKey(string s) {
		VKey = ParseVKey(s.c_str());
		oldState = 0;
	}
	buttonPress buttonCheck() {
		SHORT state = GetAsyncKeyState(VKey);
		buttonPress status = buttonPress::Unchanged;
		if ((state & 0x8000) && !(oldState & 0x8000)) {
			status = buttonPress::Down;
		}
		if (!(state & 0x8000) && (oldState & 0x8000)) {
			status = buttonPress::Up;
		}
		oldState = state;
		return status;
	}
private:
	SHORT oldState;
	int VKey;
};

WORD getXInputButton(const char* button) {
	if (_stricmp(button, "A") == 0)
		return XINPUT_GAMEPAD_A;
	if (_stricmp(button, "B") == 0)
		return XINPUT_GAMEPAD_B;
	if (_stricmp(button, "X") == 0)
		return XINPUT_GAMEPAD_X;
	if (_stricmp(button, "Y") == 0)
		return XINPUT_GAMEPAD_Y;
	if (_stricmp(button, "START") == 0)
		return XINPUT_GAMEPAD_START;
	if (_stricmp(button, "BACK") == 0)
		return XINPUT_GAMEPAD_BACK;
	if (_stricmp(button, "DPAD_RIGHT") == 0)
		return XINPUT_GAMEPAD_DPAD_RIGHT;
	if (_stricmp(button, "DPAD_LEFT") == 0)
		return XINPUT_GAMEPAD_DPAD_LEFT;
	if (_stricmp(button, "DPAD_UP") == 0)
		return XINPUT_GAMEPAD_DPAD_UP;
	if (_stricmp(button, "DPAD_DOWN") == 0)
		return XINPUT_GAMEPAD_DPAD_DOWN;
	if (_stricmp(button, "RIGHT_SHOULDER") == 0)
		return XINPUT_GAMEPAD_RIGHT_SHOULDER;
	if (_stricmp(button, "LEFT_SHOULDER") == 0)
		return XINPUT_GAMEPAD_LEFT_SHOULDER;
	if (_stricmp(button, "RIGHT_THUMB") == 0)
		return XINPUT_GAMEPAD_RIGHT_THUMB;
	if (_stricmp(button, "LEFT_THUMB") == 0)
		return XINPUT_GAMEPAD_LEFT_THUMB;
	if (_stricmp(button, "LEFT_TRIGGER") == 0)
		return 0x400;
	if (_stricmp(button, "RIGHT_TRIGGER") == 0)
		return 0x800;
	return 0;
}

class xboxKey : public button {
public:
	xboxKey(string s) {
		if (s[2] == '_') {
			c = 0;
			XKey = getXInputButton(s.c_str() + 3);
		}
		else {
			c = s[2] - '0' - 1;
			XKey = getXInputButton(s.c_str() + 4);
		}
		ZeroMemory(&oldState, sizeof(XINPUT_STATE));
		XInputGetState(c, &oldState);
	}
	buttonPress buttonCheck() {
		buttonPress status = buttonPress::Unchanged;
		XINPUT_STATE state;
		ZeroMemory(&state, sizeof(XINPUT_STATE));
		XInputGetState(c, &state);
		if (XKey == 0x400) {
			if (state.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD && oldState.Gamepad.bLeftTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
				status = buttonPress::Down;
			if (state.Gamepad.bLeftTrigger < XINPUT_GAMEPAD_TRIGGER_THRESHOLD && oldState.Gamepad.bLeftTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
				status = buttonPress::Up;
		}
		else if (XKey == 0x800) {
			if (state.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD && oldState.Gamepad.bRightTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
				status = buttonPress::Down;;
			if (state.Gamepad.bRightTrigger < XINPUT_GAMEPAD_TRIGGER_THRESHOLD && oldState.Gamepad.bRightTrigger >= XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
				status = buttonPress::Up;
		}
		else {
			if (state.Gamepad.wButtons & XKey && !(oldState.Gamepad.wButtons & XKey))
				status = buttonPress::Down;
			if (!(state.Gamepad.wButtons & XKey) && oldState.Gamepad.wButtons & XKey)
				status = buttonPress::Up;
		}
		oldState = state;
		return status;
	}
private:
	XINPUT_STATE oldState;
	WORD XKey;
	int c;
};

button* createButton(string key) {
	if (_strnicmp(key.c_str(), "XB", 2) == 0) {
		return new xboxKey(key);
	}
	else {
		return new keyboardMouseKey(key);
	}
}

string& trim(string& str)
{
	str.erase(str.begin(), find_if(str.begin(), str.end(),
		[](char& ch)->bool { return !isspace(ch); }));
	str.erase(find_if(str.rbegin(), str.rend(),
		[](char& ch)->bool { return !isspace(ch); }).base(), str.end());
	return str;
}

enum KeyType { Activate, Hold, Toggle, Cycle };
enum TransitionType { Linear, Cosine };

class ButtonHandler {
public:
	ButtonHandler(button* b, KeyType type, int variable, vector<string> value, TransitionType tt, TransitionType rtt) {
		Button = b;
		Type = type;
		Variable = variable;
		TT = tt;
		rTT = rtt;

		delay = 0;
		transition = 0;
		releaseDelay = 0;
		releaseTransition = 0;

		cyclePosition = 0;
		Value = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
		if (Type == KeyType::Cycle) {
			for (int i = 0; i < 8; i++) {
				LogInfo("%s\n", value[i].c_str());
				if (variable & 1 << i) {
					vector<float> store;
					while (true) {
						int pos = value[i].find(',');
						if (pos == value[i].npos) {
							string val = value[i];
							val = trim(val);
							if (val.size() == 0) {
								store.push_back(FLT_MAX);
							} else {
								store.push_back(stof(val));
							}
							break;
						} else {
							string val = value[i].substr(0, pos);
							val = trim(val);
							if (val.size() == 0) {
								store.push_back(FLT_MAX);
							} else {
								store.push_back(stof(val));
							}
							value[i] = value[i].substr(pos + 1);
						}
					}
					mArray.push_back(store);
				} else {
					vector<float> store;
					store.push_back(FLT_MAX);
					mArray.push_back(store);
				}
			}
			for (int i = 0; i < 8; i++) {
				maxSize = max(maxSize, mArray[i].size());
			}
			for (int i = 0; i < 8; i++) {
				if (maxSize > mArray[i].size()) {
					for (int j = mArray[i].size(); j < maxSize; j++) {
						mArray[i].push_back(mArray[i][j - 1]);
					}
				}
			}
			initializeDelay(cyclePosition);
		} else {
			if (variable & 0x001) Value[0] = stof(value[0]);
			if (variable & 0x002) Value[1] = stof(value[1]);
			if (variable & 0x004) Value[2] = stof(value[2]);
			if (variable & 0x008) Value[3] = stof(value[3]);
			if (variable & 0x010) Value[4] = stof(value[4]);
			if (variable & 0x020) Value[5] = stof(value[5]);
			if (variable & 0x040) delay = stol(value[6]);
			if (variable & 0x080) transition = stol(value[7]);
			if (variable & 0x100) releaseDelay = stol(value[8]);
			if (variable & 0x200) releaseTransition = stol(value[9]);
		}
		SavedValue = readVariable();
		toggleDown = true;

		curDelay = 0;
		curDelayUp = 0;
		curTransition = 0;
		curTransitionUp = 0;
	}
	void initializeDelay(int c) {
		delay = 0;
		if (mArray[6][c] != FLT_MAX)
			delay = mArray[6][c];
	}
	void initializeCycle(int c) {
		Variable = 0;
		for (int i = 0; i < 6; i++) {
			if (mArray[i][c] != FLT_MAX) {
				Variable |= 1 << i;
				Value[i] = mArray[i][c];
			}
		}
		transition = 0;
		if (mArray[7][c] != FLT_MAX)
			transition = mArray[7][c];
	}
	void Handle() {
		buttonPress status = Button->buttonCheck();

		if (status == buttonPress::Down) {
			if (delay > 0) {
				curDelay = GetTickCount64() + delay;
			} else {
				buttonDown();
			}
		}
		if (status == buttonPress::Up) {
			if (releaseDelay > 0) {
				curDelayUp = GetTickCount64() + releaseDelay;
			} else {
				buttonUp();
			}
		}

		if (delay > 0 && curDelay > 0 && GetTickCount64() > curDelay) {
			buttonDown();
			curDelay = 0;
		}
		if (releaseDelay > 0 && curDelayUp > 0 && GetTickCount64() > curDelayUp) {
			buttonUp();
			curDelayUp = 0;
		}
		if (transition > 0 && curTransition > 0) {
			if (GetTickCount64() > curTransition) {
				setVariable(transitionVariable(transition, curTransition, TT));
				curTransition = 0;
			} else {
				ULONGLONG newTick = GetTickCount64();
				if (newTick != lastTick) {
					setVariable(transitionVariable(transition, curTransition, TT));
					lastTick = newTick;
				}
			}
		}
		if (releaseTransition > 0 && curTransitionUp > 0) {
			if (GetTickCount64() > curTransitionUp) {
				setVariable(transitionVariable(releaseTransition, curTransitionUp, rTT));
				curTransitionUp = 0;
			} else {
				ULONGLONG newTick = GetTickCount64();
				if (newTick != lastTick) {
					setVariable(transitionVariable(releaseTransition, curTransitionUp, rTT));
					lastTick = newTick;
				}
			}
		}
	}
private:
	void buttonUp() {
		if (Type == KeyType::Hold) {
			sT = readVariable();
			Store = SavedValue;
			if (curDelay > 0)
				curDelay = 0; // cancel delayed keypress
			if (curTransition > 0)
				curTransition = 0; // cancel transition
			if (releaseTransition > 0) {
				lastTick = GetTickCount64();
				curTransitionUp = lastTick + releaseTransition;
			} else {
				setVariable(Store);
			}
		}
	}
	void buttonDown() {
		sT = readVariable();
		if (Type == KeyType::Toggle) {
			if (toggleDown) {
				if (curDelay > 0)
					curDelay = 0; // cancel delayed keypress
				if (curTransition > 0)
					curTransition = 0; // cancel transition
				else
					SavedValue = readVariable();
				toggleDown = false;
				Store = Value;
			} else {
				if (curDelay > 0)
					curDelay = 0; // cancel delayed keypress
				if (curTransition > 0)
					curTransition = 0; // cancel transition
				toggleDown = true;
				Store = SavedValue;
			}
		} else if (Type == KeyType::Hold) {
			if (curDelayUp > 0 || curDelay > 0) {
				curDelay = 0;
				curDelayUp = 0; // cancel delayed keypress
			}
			if (curTransitionUp > 0 || curTransition > 0) {
				curTransition = 0;
				curTransitionUp = 0; // cancel transition
			} else {
				SavedValue = readVariable();
			}
			Store = Value;
		} else if (Type == KeyType::Activate) {
			Store = Value;
		} else if (Type == KeyType::Cycle) {
			initializeCycle(cyclePosition++);
			if (cyclePosition == maxSize)
				cyclePosition = 0;
			initializeDelay(cyclePosition);
			Store = Value;
		}
		if (transition > 0) {
			lastTick = GetTickCount64();
			curTransition = lastTick + transition;
		} else {
			setVariable(Store);
		}
	}
	vector<float> transitionVariable(ULONGLONG transition, ULONGLONG curTransition, TransitionType tt) {
		vector<float> f(6);
		ULONGLONG transitionAmount = transition;
		if (GetTickCount64() < curTransition) {
			transitionAmount = transition - (curTransition - GetTickCount64());
		}
		float percentage = transitionAmount / (float)transition;
		if (tt == TransitionType::Cosine)
			percentage = (1 - cos(percentage * M_PI)) / 2;
		if (Variable & 0x01) f[0] = sT[0] + (Store[0] - sT[0]) * percentage;
		if (Variable & 0x02) f[1] = sT[1] + (Store[1] - sT[1]) * percentage;
		if (Variable & 0x04) f[2] = sT[2] + (Store[2] - sT[2]) * percentage;
		if (Variable & 0x08) f[3] = sT[3] + (Store[3] - sT[3]) * percentage;
		if (Variable & 0x10) f[4] = sT[4] + (Store[4] - sT[4]) * percentage;
		if (Variable & 0x20) f[5] = sT[5] + (Store[5] - sT[5]) * percentage;
		return f;
	}
	vector<float> readVariable() {
		vector<float> f(6);
		if (Variable & 0x01) f[0] = iniParams[0].x;
		if (Variable & 0x02) f[1] = iniParams[0].y;
		if (Variable & 0x04) f[2] = iniParams[0].z;
		if (Variable & 0x08) f[3] = iniParams[0].w;
		if (Variable & 0x10) NvAPI_Stereo_GetConvergence(gStereoHandle, &f[4]);
		if (Variable & 0x20) NvAPI_Stereo_GetSeparation(gStereoHandle, &f[5]);
		return f;
	}
	void setVariable(vector<float> f) {
		if (Variable & 0x01) iniParams[0].x = f[0];
		if (Variable & 0x02) iniParams[0].y = f[1];
		if (Variable & 0x04) iniParams[0].z = f[2];
		if (Variable & 0x08) iniParams[0].w = f[3];
		if (Variable & 0x10) {
			if (gl_nvapi)
				NvAPIOverride();
			NvAPI_Stereo_SetConvergence(gStereoHandle, f[4]);
		}
		if (Variable & 0x20) {
			if (gl_nvapi)
				NvAPIOverride();
			NvAPI_Stereo_SetSeparation(gStereoHandle, f[5]);
		}
		if (Variable & 0x0F) {
			D3D11_MAPPED_SUBRESOURCE mappedResource;
			gContext->Map(gIniTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
			memcpy(mappedResource.pData, &iniParams, sizeof(iniParams));
			gContext->Unmap(gIniTexture, 0);
		}
	}
	button* Button;
	KeyType Type;
	// Variable Flags
	// 1 INIParams.x
	// 2 INIParams.y
	// 4 INIParams.z
	// 8 INIParams.w
	// 16 Convergence
	// 32 Separation
	int Variable;
	TransitionType TT;
	TransitionType rTT;
	vector<float> Value;
	vector<float> SavedValue;
	vector<float> Store;
	vector<float> sT; // start transition
	ULONGLONG lastTick;

	ULONGLONG delay;
	ULONGLONG releaseDelay;
	ULONGLONG curDelay;
	ULONGLONG curDelayUp;

	ULONGLONG transition;
	ULONGLONG releaseTransition;
	ULONGLONG curTransition;
	ULONGLONG curTransitionUp;
	bool toggleDown;
	int cyclePosition;
	vector<vector<float>> mArray;
	int maxSize = 0;
};

vector<ButtonHandler*> BHs;

void frameFunction() {
	for (size_t i = 0; i < BHs.size(); i++) {
		BHs[i]->Handle();
	}
}
#pragma endregion

#pragma region DXGI
HRESULT STDMETHODCALLTYPE DXGIH_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
	frameFunction();
	if (gDevice && gContext) {
		gStereoTexMgr->UpdateStereoTexture(gDevice, gContext, gStereoTexture, false);
	}
	return sDXGI_Present_Hook.fnDXGI_Present(This, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DXGIH_ResizeBuffers(IDXGISwapChain* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
	HRESULT hr = sDXGI_ResizeBuffers_Hook.fnDXGI_ResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);

	if (SUCCEEDED(hr) && gResolutionInfo.from == GetResolutionFrom::SWAP_CHAIN) {
		gResolutionInfo.width = Width;
		gResolutionInfo.height = Height;
		LogInfo("Got resolution from swap chain: %ix%i\n",
			gResolutionInfo.width, gResolutionInfo.height);
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE DXGI_CreateSwapChain1(IDXGIFactory1 * This, IUnknown * pDevice, DXGI_SWAP_CHAIN_DESC * pDesc, IDXGISwapChain ** ppSwapChain) {
	LogInfo("CreateSwapChain\n");
	HRESULT hr = sCreateSwapChain_Hook.fnCreateSwapChain1(This, pDevice, pDesc, ppSwapChain);
	if (!gl_Present_hooked) {
		LogInfo("Present hooked\n");
		gl_Present_hooked = true;
		DWORD_PTR*** vTable = (DWORD_PTR***)*ppSwapChain;
		DXGI_Present origPresent = (DXGI_Present)(*vTable)[8];
		DXGI_ResizeBuffers origResizeBuffers = (DXGI_ResizeBuffers)(*vTable)[13];
		cHookMgr.Hook(&(sDXGI_Present_Hook.nHookId), (LPVOID*)&(sDXGI_Present_Hook.fnDXGI_Present), origPresent, DXGIH_Present);
		cHookMgr.Hook(&(sDXGI_ResizeBuffers_Hook.nHookId), (LPVOID*)&(sDXGI_ResizeBuffers_Hook.fnDXGI_ResizeBuffers), origResizeBuffers, DXGIH_ResizeBuffers);

		if (pDesc && gResolutionInfo.from == GetResolutionFrom::SWAP_CHAIN) {
			gResolutionInfo.width = pDesc->BufferDesc.Width;
			gResolutionInfo.height = pDesc->BufferDesc.Height;
			LogInfo("Got resolution from swap chain: %ix%i\n",
				gResolutionInfo.width, gResolutionInfo.height);
		}
	}
	return hr;
}

void HackedPresent(ID3D11Device *pDevice) {
	IDXGIFactory1 * pFactory;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&pFactory));
	DWORD_PTR*** vTable = (DWORD_PTR***)pFactory;
	DXGI_CSC1 origCSC1 = (DXGI_CSC1)(*vTable)[10];
	cHookMgr.Hook(&(sCreateSwapChain_Hook.nHookId), (LPVOID*)&(sCreateSwapChain_Hook.fnCreateSwapChain1), origCSC1, DXGI_CreateSwapChain1);
	pFactory->Release();
}
#pragma endregion

#pragma region Hooks
HRESULT CreateStereoParamTextureAndView(ID3D11Device* d3d11)
{
	// This function creates a texture that is suitable to be stereoized by the driver.
	// Note that the parameters primarily come from nvstereo.h
	using nv::stereo::ParamTextureManagerD3D11;

	HRESULT hr = 0;

	D3D11_TEXTURE2D_DESC desc;
	desc.Width = ParamTextureManagerD3D11::Parms::StereoTexWidth;
	desc.Height = ParamTextureManagerD3D11::Parms::StereoTexHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = ParamTextureManagerD3D11::Parms::StereoTexFormat;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = 0;
	d3d11->CreateTexture2D(&desc, NULL, &gStereoTexture);

	// Since we need to bind the texture to a shader input, we also need a resource view.
	D3D11_SHADER_RESOURCE_VIEW_DESC descRV;
	descRV.Format = desc.Format;
	descRV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	descRV.Texture2D.MipLevels = 1;
	descRV.Texture2D.MostDetailedMip = 0;
	descRV.Texture2DArray.MostDetailedMip = 0;
	descRV.Texture2DArray.MipLevels = 1;
	descRV.Texture2DArray.FirstArraySlice = 0;
	descRV.Texture2DArray.ArraySize = desc.ArraySize;
	d3d11->CreateShaderResourceView(gStereoTexture, &descRV, &gStereoResourceView);

	return S_OK;
}

void CreateINITexture(ID3D11Device* d3d11) {
	if (gIniTexture != 0) {
		gIniTexture->Release();
	}

	D3D11_TEXTURE1D_DESC desc;
	memset(&desc, 0, sizeof(D3D11_TEXTURE1D_DESC));
	D3D11_SUBRESOURCE_DATA initialData;
	initialData.pSysMem = &iniParams;
	initialData.SysMemPitch = sizeof(DirectX::XMFLOAT4) * INI_PARAMS_SIZE;	// only one 4 element struct

	desc.Width = 1;												// 1 texel, .rgba as a float4
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;	// float4
	desc.Usage = D3D11_USAGE_DYNAMIC;				// Read/Write access from GPU and CPU
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;		// As resource view, access via t120
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;		// allow CPU access for hotkeys
	desc.MiscFlags = 0;
	HRESULT ret = d3d11->CreateTexture1D(&desc, &initialData, &gIniTexture);
	// Since we need to bind the texture to a shader input, we also need a resource view.
	// The pDesc is set to NULL so that it will simply use the desc format above.
	D3D11_SHADER_RESOURCE_VIEW_DESC descRV;
	memset(&descRV, 0, sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC));
	ret = d3d11->CreateShaderResourceView(gIniTexture, NULL, &gIniResourceView);
}

void InitializeStereo(ID3D11Device * pDevice) {
	gDevice = pDevice;
	if (NVAPI_OK != NvAPI_Stereo_CreateHandleFromIUnknown(gDevice, &gStereoHandle))
		gStereoHandle = 0;
	CreateINITexture(gDevice);
	// Create our stereo parameter texture
	CreateStereoParamTextureAndView(gDevice);
	// Initialize the stereo texture manager. Note that the StereoTextureManager was created
	// before the device. This is important, because NvAPI_Stereo_CreateConfigurationProfileRegistryKey
	// must be called BEFORE device creation.
	gStereoTexMgr->Init(gDevice);
	LogInfo("Stereo Initialized\n");
}

void hook(ID3D11DeviceContext** ppContext) {
	if (ppContext != NULL && *ppContext != NULL) {
		LogInfo("Context Hook: %p\n", *ppContext);
		if (!gl_hookedContext) {
			gl_hookedContext = true;
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppContext;
			D3D11C_PSSS origPSSS = (D3D11C_PSSS)(*vTable)[9];
			D3D11C_VSSS origVSSS = (D3D11C_VSSS)(*vTable)[11];
			D3D11C_GSSS origGSSS = (D3D11C_GSSS)(*vTable)[23];
			D3D11C_HSSS origHSSS = (D3D11C_HSSS)(*vTable)[60];
			D3D11C_DSSS origDSSS = (D3D11C_DSSS)(*vTable)[64];
			D3D11C_CSSS origCSSS = (D3D11C_CSSS)(*vTable)[69];

			cHookMgr.Hook(&(sPSSetShader_Hook.nHookId), (LPVOID*)&(sPSSetShader_Hook.fnPSSetShader), origPSSS, D3D11C_PSSetShader);
			cHookMgr.Hook(&(sVSSetShader_Hook.nHookId), (LPVOID*)&(sVSSetShader_Hook.fnVSSetShader), origVSSS, D3D11C_VSSetShader);
			cHookMgr.Hook(&(sGSSetShader_Hook.nHookId), (LPVOID*)&(sGSSetShader_Hook.fnGSSetShader), origGSSS, D3D11C_GSSetShader);
			cHookMgr.Hook(&(sHSSetShader_Hook.nHookId), (LPVOID*)&(sHSSetShader_Hook.fnHSSetShader), origHSSS, D3D11C_HSSetShader);
			cHookMgr.Hook(&(sDSSetShader_Hook.nHookId), (LPVOID*)&(sDSSetShader_Hook.fnDSSetShader), origDSSS, D3D11C_DSSetShader);
			cHookMgr.Hook(&(sCSSetShader_Hook.nHookId), (LPVOID*)&(sCSSetShader_Hook.fnCSSetShader), origCSSS, D3D11C_CSSetShader);

			gContext = *ppContext;

			LogInfo("Context COM hooked\n");
		}
	}
}

void hook(ID3D11Device** ppDevice) {
	if (ppDevice != NULL && *ppDevice != NULL) {
		LogInfo("Device Hook: %p\n", *ppDevice);
		if (!gl_hookedDevice) {
			gl_hookedDevice = true;
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppDevice;
			D3D11_VS origVS = (D3D11_VS)(*vTable)[12];
			D3D11_PS origPS = (D3D11_PS)(*vTable)[15];
			D3D11_GS origGS = (D3D11_GS)(*vTable)[13];
			D3D11_HS origHS = (D3D11_HS)(*vTable)[16];
			D3D11_DS origDS = (D3D11_DS)(*vTable)[17];
			D3D11_CS origCS = (D3D11_CS)(*vTable)[18];

			cHookMgr.Hook(&(sCreateVertexShader_Hook.nHookId), (LPVOID*)&(sCreateVertexShader_Hook.fnCreateVertexShader), origVS, D3D11_CreateVertexShader);
			cHookMgr.Hook(&(sCreatePixelShader_Hook.nHookId), (LPVOID*)&(sCreatePixelShader_Hook.fnCreatePixelShader), origPS, D3D11_CreatePixelShader);
			cHookMgr.Hook(&(sCreateGeometryShader_Hook.nHookId), (LPVOID*)&(sCreateGeometryShader_Hook.fnCreateGeometryShader), origGS, D3D11_CreateGeometryShader);
			cHookMgr.Hook(&(sCreateHullShader_Hook.nHookId), (LPVOID*)&(sCreateHullShader_Hook.fnCreateHullShader), origHS, D3D11_CreateHullShader);
			cHookMgr.Hook(&(sCreateDomainShader_Hook.nHookId), (LPVOID*)&(sCreateDomainShader_Hook.fnCreateDomainShader), origDS, D3D11_CreateDomainShader);
			cHookMgr.Hook(&(sCreateComputeShader_Hook.nHookId), (LPVOID*)&(sCreateComputeShader_Hook.fnCreateComputeShader), origCS, D3D11_CreateComputeShader);
			LogInfo("Device COM hooked\n");

			HackedPresent(*ppDevice);
		}
		InitializeStereo(*ppDevice);
	}
}
#pragma endregion

#pragma region exports
// Exported function (faking d3d11.dll's export)
HRESULT WINAPI D3D11CreateDevice(
	_In_   IDXGIAdapter *pAdapter,
	_In_   D3D_DRIVER_TYPE DriverType,
	_In_   HMODULE Software,
	_In_   UINT Flags,
	_In_   const D3D_FEATURE_LEVEL *pFeatureLevels,
	_In_   UINT FeatureLevels,
	_In_   UINT SDKVersion,
	_Out_  ID3D11Device **ppDevice,
	_Out_  D3D_FEATURE_LEVEL *pFeatureLevel,
	_Out_  ID3D11DeviceContext **ppImmediateContext
	)
{
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d11.dll"
	
	// Hooking IDirect3D Object from Original Library
	typedef HRESULT (WINAPI* D3D11_Type)(
	IDXGIAdapter *pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	const D3D_FEATURE_LEVEL *pFeatureLevels,
	UINT FeatureLevels,
	UINT SDKVersion,
	ID3D11Device **ppDevice,
	D3D_FEATURE_LEVEL *pFeatureLevel,
	ID3D11DeviceContext **ppImmediateContext
	);
	D3D11_Type D3D11CreateDevice_fn = (D3D11_Type) GetProcAddress( gl_hOriginalDll, "D3D11CreateDevice");
	HRESULT res = D3D11CreateDevice_fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
	if (res == 0) {
		hook(ppDevice);
		hook(ppImmediateContext);
	}
	return res;
}
HRESULT WINAPI D3D11CreateDeviceAndSwapChain(
	_In_   IDXGIAdapter *pAdapter,
	_In_   D3D_DRIVER_TYPE DriverType,
	_In_   HMODULE Software,
	_In_   UINT Flags,
	_In_   const D3D_FEATURE_LEVEL *pFeatureLevels,
	_In_   UINT FeatureLevels,
	_In_   UINT SDKVersion,
	_In_   const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
	_Out_  IDXGISwapChain **ppSwapChain,
	_Out_  ID3D11Device **ppDevice,
	_Out_  D3D_FEATURE_LEVEL *pFeatureLevel,
	_Out_  ID3D11DeviceContext **ppImmediateContext
	)
{
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d11.dll"

	// Hooking IDirect3D Object from Original Library
	typedef HRESULT(WINAPI* D3D11_Type)(
		IDXGIAdapter *pAdapter,
		D3D_DRIVER_TYPE DriverType,
		HMODULE Software,
		INT Flags,
		const D3D_FEATURE_LEVEL *pFeatureLevels,
		UINT FeatureLevels,
		UINT SDKVersion,
		const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
		IDXGISwapChain **ppSwapChain,
		ID3D11Device **ppDevice,
		D3D_FEATURE_LEVEL *pFeatureLevel,
		ID3D11DeviceContext **ppImmediateContext
		);
	D3D11_Type D3D11CreateDeviceAndSwapChain_fn = (D3D11_Type)GetProcAddress(gl_hOriginalDll, "D3D11CreateDeviceAndSwapChain");
	HRESULT res = D3D11CreateDeviceAndSwapChain_fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
	if (res == 0) {
		hook(ppDevice);
		hook(ppImmediateContext);
	}
	return res;
}
#pragma endregion

void ShowStartupScreen()
{
	BOOL affinity = -1;
	DWORD_PTR one = 0x01;
	DWORD_PTR before = 0;
	DWORD_PTR before2 = 0;
	affinity = GetProcessAffinityMask(GetCurrentProcess(), &before, &before2);
	affinity = SetProcessAffinityMask(GetCurrentProcess(), one);
	HBITMAP hBM = ::LoadBitmap(gl_hThisInstance, MAKEINTRESOURCE(IDB_STARTUP));
	if (hBM) {
		HDC hDC = ::GetDC(NULL);
		if (hDC) {
			int iXPos = (::GetDeviceCaps(hDC, HORZRES) / 2) - (128 / 2);
			int iYPos = (::GetDeviceCaps(hDC, VERTRES) / 2) - (128 / 2);

			// paint the "GPP active" sign on desktop
			HDC hMemDC = ::CreateCompatibleDC(hDC);
			HBITMAP hBMold = (HBITMAP) ::SelectObject(hMemDC, hBM);
			::BitBlt(hDC, iXPos, iYPos, 128, 128, hMemDC, 0, 0, SRCCOPY);

			//Cleanup
			::SelectObject(hMemDC, hBMold);
			::DeleteDC(hMemDC);
			::ReleaseDC(NULL, hDC);

			// Wait 1 seconds before proceeding
			::Sleep(2000);
		}
		::DeleteObject(hBM);
	}
	affinity = SetProcessAffinityMask(GetCurrentProcess(), before);
}

#pragma region INI

void InitInstance()
{
	// Initialisation
	gl_hOriginalDll = NULL;

	char setting[MAX_PATH];
	char iniFile[MAX_PATH];
	char LOGfile[MAX_PATH];

	_getcwd(iniFile, MAX_PATH);
	_getcwd(LOGfile, MAX_PATH);
	_getcwd(cwd, MAX_PATH);
	strcat_s(iniFile, MAX_PATH, "\\d3dx.ini");

	// If specified in Debug section, wait for Attach to Debugger.
	bool waitfordebugger = GetPrivateProfileInt("Debug", "attach", 0, iniFile) > 0;
	if (waitfordebugger) {
		do {
			Sleep(250);
		} while (!IsDebuggerPresent());
	}

	gl_log = GetPrivateProfileInt("Logging", "calls", gl_log, iniFile) > 0;
	gl_dump = GetPrivateProfileInt("Rendering", "export_binary", gl_dump, iniFile) > 0;
	gl_cache_shaders = GetPrivateProfileInt("Rendering", "cache_shaders", gl_cache_shaders, iniFile) > 0;

	if (gl_log) {
		strcat_s(LOGfile, MAX_PATH, "\\d3d11_log.txt");
		LogFile = _fsopen(LOGfile, "wb", _SH_DENYNO);
		setvbuf(LogFile, NULL, _IONBF, 0);
		LogInfo("Start Log:\n");
	}

	// Read in any constants defined in the ini, for use as shader parameters
	// Any result of the default FLT_MAX means the parameter is not in use.
	// stof will crash if passed FLT_MAX, hence the extra check.
	// We use FLT_MAX instead of the more logical INFINITY, because Microsoft *always* generates 
	// warnings, even for simple comparisons. And NaN comparisons are similarly broken.
	LogInfo("[Constants]\n");
	for (int i = 0; i < INI_PARAMS_SIZE; i++) {
		char buf[8];
		iniParams[i].x = FLT_MAX;
		iniParams[i].y = FLT_MAX;
		iniParams[i].z = FLT_MAX;
		iniParams[i].w = FLT_MAX;
		_snprintf_s(buf, 8, "x%.0i", i);
		if (GetPrivateProfileString("Constants", buf, "FLT_MAX", setting, MAX_PATH, iniFile))
		{
			if (strcmp(setting, "FLT_MAX") != 0) {
				iniParams[i].x = stof(setting);
				LogInfo("  %s=%#.2g\n", buf, iniParams[i].x);
			}
		}
		_snprintf_s(buf, 8, "y%.0i", i);
		if (GetPrivateProfileString("Constants", buf, "FLT_MAX", setting, MAX_PATH, iniFile))
		{
			if (strcmp(setting, "FLT_MAX") != 0) {
				iniParams[i].y = stof(setting);
				LogInfo("  %s=%#.2g\n", buf, iniParams[i].y);
			}
		}
		_snprintf_s(buf, 8, "z%.0i", i);
		if (GetPrivateProfileString("Constants", buf, "FLT_MAX", setting, MAX_PATH, iniFile))
		{
			if (strcmp(setting, "FLT_MAX") != 0) {
				iniParams[i].z = stof(setting);
				LogInfo("  %s=%#.2g\n", buf, iniParams[i].z);
			}
		}
		_snprintf_s(buf, 8, "w%.0i", i);
		if (GetPrivateProfileString("Constants", buf, "FLT_MAX", setting, MAX_PATH, iniFile))
		{
			if (strcmp(setting, "FLT_MAX") != 0) {
				iniParams[i].w = stof(setting);
				LogInfo("  %s=%#.2g\n", buf, iniParams[i].w);
			}
		}
	}
	if (GetPrivateProfileString("Device", "get_resolution_from", 0, setting, MAX_PATH, iniFile)) {
		if (_stricmp(setting, "swap_chain") == 0)
			gResolutionInfo.from = GetResolutionFrom::SWAP_CHAIN;
		if (_stricmp(setting, "depth_stencil") == 0)
			gResolutionInfo.from = GetResolutionFrom::DEPTH_STENCIL;
	}

	KeyType type;
	char key[MAX_PATH];
	char buf[MAX_PATH];

	vector<string> Keys;
	vector<string> Shaders;
	vector<string> Textures;
	char sectionNames[10000];
	GetPrivateProfileSectionNames(sectionNames, 10000, iniFile);
	size_t position = 0;
	size_t length = strlen(&sectionNames[position]);
	while (length != 0) {
		if (strncmp(&sectionNames[position], "Key", 3) == 0)
			Keys.push_back(&sectionNames[position]);
		if (strncmp(&sectionNames[position], "ShaderOverride", 14) == 0)
			Shaders.push_back(&sectionNames[position]);
		if (strncmp(&sectionNames[position], "TextureOverride", 15) == 0)
			Textures.push_back(&sectionNames[position]);
		position += length + 1;
		length = strlen(&sectionNames[position]);
	}

	for (size_t i = 0; i < Keys.size(); i++) {
		const char* id = Keys[i].c_str();
		if (!GetPrivateProfileString(id, "Key", 0, key, MAX_PATH, iniFile))
			continue;

		type = KeyType::Activate;

		if (GetPrivateProfileString(id, "type", 0, buf, MAX_PATH, iniFile)) {
			if (!_stricmp(buf, "hold")) {
				type = KeyType::Hold;
			}
			else if (!_stricmp(buf, "toggle")) {
				type = KeyType::Toggle;
			}
			else if (!_stricmp(buf, "cycle")) {
				type = KeyType::Cycle;
			}
		}

		TransitionType tType = TransitionType::Linear;
		if (GetPrivateProfileString(id, "transition_type", 0, buf, MAX_PATH, iniFile)) {
			if (!_stricmp(buf, "cosine"))
				tType = TransitionType::Cosine;
		}

		TransitionType rtType = TransitionType::Linear;
		if (GetPrivateProfileString(id, "release_transition_type", 0, buf, MAX_PATH, iniFile)) {
			if (!_stricmp(buf, "cosine"))
				rtType = TransitionType::Cosine;
		}

		vector<string> fs = { "", "", "", "", "", "", "", "", "", "" };
		int varFlags = 0;

		if (GetPrivateProfileString(id, "x", 0, buf, MAX_PATH, iniFile)) {
			fs[0] = buf;
			varFlags |= 1;
		}
		if (GetPrivateProfileString(id, "y", 0, buf, MAX_PATH, iniFile)) {
			fs[1] = buf;
			varFlags |= 2;
		}
		if (GetPrivateProfileString(id, "z", 0, buf, MAX_PATH, iniFile)) {
			fs[2] = buf;
			varFlags |= 4;
		}
		if (GetPrivateProfileString(id, "w", 0, buf, MAX_PATH, iniFile)) {
			fs[3] = buf;
			varFlags |= 8;
		}
		if (GetPrivateProfileString(id, "convergence", 0, buf, MAX_PATH, iniFile)) {
			fs[4] = buf;
			varFlags |= 16;
		}
		if (GetPrivateProfileString(id, "separation", 0, buf, MAX_PATH, iniFile)) {
			fs[5] = buf;
			varFlags |= 32;
		}
		if (GetPrivateProfileString(id, "delay", 0, buf, MAX_PATH, iniFile)) {
			fs[6] = buf;
			varFlags |= 64;
		}
		if (GetPrivateProfileString(id, "transition", 0, buf, MAX_PATH, iniFile)) {
			fs[7] = buf;
			varFlags |= 128;
		}
		if (GetPrivateProfileString(id, "release_delay", 0, buf, MAX_PATH, iniFile)) {
			fs[8] = buf;
			varFlags |= 256;
		}
		if (GetPrivateProfileString(id, "release_transition", 0, buf, MAX_PATH, iniFile)) {
			fs[9] = buf;
			varFlags |= 512;
		}
		BHs.push_back(new ButtonHandler(createButton(key), type, varFlags, fs, tType, rtType));
	}

	InitializeCriticalSection(&gl_CS);

	gStereoTexMgr = new nv::stereo::ParamTextureManagerD3D11;

	WIN32_FIND_DATA findFileData;

	HANDLE hFind = FindFirstFile("ShaderFixes\\????????????????-??.bin", &findFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			string s = findFileData.cFileName;
			string sHash = s.substr(0, 16);
			UINT64 _crc = stoull(sHash, NULL, 16);
			isCache[_crc] = true;
		} while (FindNextFile(hFind, &findFileData));
		FindClose(hFind);
	}

	hFind = FindFirstFile("ShaderFixes\\????????????????-??.txt", &findFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			string s = findFileData.cFileName;
			string sHash = s.substr(0, 16);
			UINT64 _crc = stoull(sHash, NULL, 16);
			hasStartPatch[_crc] = true;
		} while (FindNextFile(hFind, &findFileData));
		FindClose(hFind);
	}

	hFind = FindFirstFile("ShaderFixes\\????????????????-??_replace.txt", &findFileData);
	if (hFind != INVALID_HANDLE_VALUE) {
		do {
			string s = findFileData.cFileName;
			string sHash = s.substr(0, 16);
			UINT64 _crc = stoull(sHash, NULL, 16);
			hasStartFix[_crc] = true;
		} while (FindNextFile(hFind, &findFileData));
		FindClose(hFind);
	}
}
#pragma endregion

void LoadOriginalDll(void)
{
	if (!gl_hOriginalDll) gl_hOriginalDll = Hooked_LoadLibraryExW(L"original_d3d11.dll", NULL, 0);
}

void ExitInstance() 
{    
	if (gl_hOriginalDll)
	{
		::FreeLibrary(gl_hOriginalDll);
	    gl_hOriginalDll = NULL;  
	}
}

extern "C" NvAPI_Status __cdecl nvapi_QueryInterface(unsigned int offset);

void NvAPIOverride() {
	// One shot, override custom settings.
	NvAPI_Status ret = nvapi_QueryInterface(0xb03bb03b);
	if (ret != 0xeecc34ab)
		LogInfo("  overriding NVAPI wrapper failed. \n");
}
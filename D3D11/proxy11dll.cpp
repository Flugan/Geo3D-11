// proxydll.cpp
#include "proxy11dll.h"
#include "Nektra\NktHookLib.h"
#include "log.h"
#include <map>
#include <DirectXMath.h>
#include <D3Dcompiler.h>
#include <algorithm>
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
bool				gl_left = true;
char				cwd[MAX_PATH];
FILE *LogFile = 0;		// off by default.
bool gLogDebug = false;
CRITICAL_SECTION	gl_CS;
const int INI_PARAMS_SIZE = 16;
DirectX::XMFLOAT4	iniParams[INI_PARAMS_SIZE];
ID3D11Texture2D *gStereoTextureLeft = NULL;
ID3D11ShaderResourceView* gStereoResourceViewLeft = NULL;
ID3D11Texture2D* gStereoTextureRight = NULL;
ID3D11ShaderResourceView* gStereoResourceViewRight = NULL;
ID3D11Texture1D *gIniTexture = NULL;
ID3D11ShaderResourceView *gIniResourceView = NULL;

float gSep;
float gConv;
float gEyeDist;
float gScreenSize;
float gFinalSep;

map<UINT64, bool> hasStartPatch;
map<UINT64, bool> hasStartFix;
#pragma data_seg ()

CNktHookLib cHookMgr;



BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	bool result = true;

	switch (fdwReason) {
	case DLL_PROCESS_ATTACH:
		gl_hThisInstance = hinstDLL;
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
string changeASM(vector<byte> ASM, bool left) {
	auto lines = stringToLines((char*)ASM.data(), ASM.size());
	string shader;
	string oReg;
	bool dcl = false;
	bool dcl_ICB = false;
	int temp = 0;
	for (int i = 0; i < lines.size(); i++) {
		string s = lines[i];
		if (s.find("dcl") == 0) {
			dcl = true;
			dcl_ICB = false;
			if (s.find("dcl_output_siv") == 0 && s.find("position") != string::npos) {
				oReg = s.substr(15, 2);
				shader += s + "\n";
			}
			else if (s.find("dcl_temps") == 0) {
				string num = s.substr(10);
				temp = atoi(num.c_str()) + 2;
				shader += "dcl_temps " + to_string(temp) + "\n";
			}
			else if (s.find("dcl_immediateConstantBuffer") == 0) {
				dcl_ICB = true;
				shader += s + "\n";
			}
			else {
				shader += s + "\n";
			}
		}
		else if (dcl_ICB == true) {
			shader += s + "\n";
		}
		else if (dcl == true) {
			// after dcl
			if (s.find("ret") < s.size()) {
				char buf[80];
				sprintf_s(buf, 80, "%.8f", gFinalSep);
				string sep(buf);
				sprintf_s(buf, 80, "%.3f", gConv);
				string conv(buf);
				string changeSep = left ? "l(-" + sep + ")" : "l(" + sep + ")";
				shader +=
					"eq r" + to_string(temp - 2) + ".x, r" + to_string(temp - 1) + ".w, l(1.0)\n" +
					"if_z r" + to_string(temp - 2) + ".x\n"
					"  add r" + to_string(temp - 2) + ".x, r" + to_string(temp - 1) + ".w, l(-" + conv + ")\n" +
					"  mad r" + to_string(temp - 2) + ".x, r" + to_string(temp - 2) + ".x, " + changeSep + ", r" + to_string(temp - 1) + ".x\n" +
					"  mov " + oReg + ".x, r" + to_string(temp - 2) + ".x\n" +
					"  ret\n" +
					"endif\n";
			}
			if (oReg.size() == 0) {
				// no output
				return "";
			}
			if (temp == 0) {
				// add temps
				temp = 2;
				shader += "dcl_temps 2\n";
			}
			shader += s + "\n";
			auto pos = s.find(oReg);
			if (pos != string::npos) {
				string reg = "r" + to_string(temp - 1);
				for (int i = 0; i < s.size(); i++) {
					if (i < pos) {
						shader += s[i];
					}
					else if (i == pos) {
						shader += reg;
					}
					else if (i > pos + 1) {
						shader += s[i];
					}
				}
				shader += "\n";
			}
		}
		else {
			// before dcl
			shader += s + "\n";
		}
	}
	return shader;
}

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
vector<byte> assembled(char* buffer, const void* pShaderBytecode, SIZE_T BytecodeLength) {
	char path[MAX_PATH];
	path[0] = 0;
	strcat_s(path, MAX_PATH, cwd);
	strcat_s(path, MAX_PATH, "\\ShaderFixes\\");
	strcat_s(path, MAX_PATH, buffer);
	strcat_s(path, MAX_PATH, ".txt");
	LogInfo("loaded: %s\n", path);
	auto file = readFile(path);

	vector<byte>* v = new vector<byte>(BytecodeLength);
	copy((byte*)pShaderBytecode, (byte*)pShaderBytecode + BytecodeLength, v->begin());

	vector<byte> byteCode = assembler(file, *v);
	return byteCode;
}
ID3DBlob* hlsled(char* buffer, char* shdModel){
	char path[MAX_PATH];
	path[0] = 0;
	strcat_s(path, MAX_PATH, cwd);
	strcat_s(path, MAX_PATH, "\\ShaderFixes\\");
	strcat_s(path, MAX_PATH, buffer);
	strcat_s(path, MAX_PATH, "_replace.txt");
	LogInfo("loaded: %s\n", path);
	auto file = readFile(path);

	ID3DBlob* pByteCode = nullptr;
	ID3DBlob* pErrorMsgs = nullptr;
	HRESULT ret = D3DCompile(file.data(), file.size(), NULL, 0, ((ID3DInclude*)(UINT_PTR)1),
		"main", shdModel, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pByteCode, &pErrorMsgs);
	return pByteCode;
}

HRESULT STDMETHODCALLTYPE D3D11_CreateVertexShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create VertexShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-vs", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT hr;
	byte* bArray;
	SIZE_T bSize;
	if (hasStartPatch.count(_crc)) {
		auto data = assembled(buffer, pShaderBytecode, BytecodeLength);
		bArray = (byte*)data.data();
		bSize = data.size();
	} else if (hasStartFix.count(_crc)) {
		ID3DBlob* pByteCode = hlsled(buffer, "vs_5_0");
		bArray = (byte*)pByteCode->GetBufferPointer();
		bSize = pByteCode->GetBufferSize();
	} else {
		bArray = (byte*)pShaderBytecode;
		bSize = BytecodeLength;
	}
	vector<byte> v;
	for (int i = 0; i < bSize; i++) {
		v.push_back(bArray[i]);
	}
	vector<byte> ASM = disassembler(v);

	string shaderL = changeASM(ASM, true);
	string shaderR = changeASM(ASM, false);

	if (shaderL == "") {
		hr = sCreateVertexShader_Hook.fnCreateVertexShader(This, pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader);
		VSO vso = {};
		vso.Neutral = (ID3D11VertexShader*)*ppVertexShader;
		VSOmap[vso.Neutral] = vso;
		return hr;
	}

	vector<byte> a;
	VSO vso = {};

	a.clear();
	for (int i = 0; i < shaderL.length(); i++) {
		a.push_back(shaderL[i]);
	}
	auto compiled = assembler(a, v);
	hr = sCreateVertexShader_Hook.fnCreateVertexShader(This, compiled.data(), compiled.size(), pClassLinkage, ppVertexShader);
	vso.Left = (ID3D11VertexShader*)*ppVertexShader;

	a.clear();
	for (int i = 0; i < shaderR.length(); i++) {
		a.push_back(shaderR[i]);
	}
	compiled = assembler(a, v);
	hr = sCreateVertexShader_Hook.fnCreateVertexShader(This, compiled.data(), compiled.size(), pClassLinkage, ppVertexShader);
	vso.Right = (ID3D11VertexShader*)*ppVertexShader;
	VSOmap[vso.Right] = vso;
	return hr;
}

HRESULT STDMETHODCALLTYPE D3D11_CreatePixelShader(ID3D11Device * This, const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader) {
	UINT64 _crc = fnv_64_buf(pShaderBytecode, BytecodeLength);
	LogInfo("Create PixelShader: %016llX\n", _crc);

	char buffer[80];
	sprintf_s(buffer, 80, "%016llX-ps", _crc);
	if (gl_dump)
		dump(pShaderBytecode, BytecodeLength, buffer);
	HRESULT res;
	if (hasStartPatch.count(_crc)) {
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
	if (hasStartPatch.count(_crc)) {
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
	if (hasStartPatch.count(_crc)) {
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
	if (hasStartPatch.count(_crc)) {
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
	if (hasStartPatch.count(_crc)) {
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

void STDMETHODCALLTYPE D3D11_GetImmediateContext(ID3D11Device* This, ID3D11DeviceContext** ppImmediateContext) {
	sGetImmediateContext_Hook.fn(This, ppImmediateContext);
	hook(ppImmediateContext);
}

#pragma region SetShader
void STDMETHODCALLTYPE D3D11C_VSSetShader(ID3D11DeviceContext* This, ID3D11VertexShader* pVertexShader, ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances) {
	if (VSOmap.count(pVertexShader) == 1) {
		VSO* vso = &VSOmap[pVertexShader];
		if (vso->Neutral) {
			LogInfo("No output VS\n");
			sVSSetShader_Hook.fnVSSetShader(This, vso->Neutral, ppClassInstances, NumClassInstances);
		}
		else {
			LogInfo("Stereo VS\n");
			if (gl_left) {
				sVSSetShader_Hook.fnVSSetShader(This, vso->Left, ppClassInstances, NumClassInstances);
			}
			else {
				sVSSetShader_Hook.fnVSSetShader(This, vso->Right, ppClassInstances, NumClassInstances);
			}
		}
	}
	else {
		LogInfo("Unknown VS\n");
		sVSSetShader_Hook.fnVSSetShader(This, pVertexShader, ppClassInstances, NumClassInstances);
	}
	if (gStereoTextureLeft > 0) {
		if (gl_left)
			This->VSSetShaderResources(125, 1, &gStereoResourceViewLeft);
		else
			This->VSSetShaderResources(125, 1, &gStereoResourceViewRight);
	}
	if (gIniTexture > 0)
		This->VSSetShaderResources(120, 1, &gIniResourceView);
}

void STDMETHODCALLTYPE D3D11C_PSSetShader(ID3D11DeviceContext * This, ID3D11PixelShader *pPixelShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sPSSetShader_Hook.fnPSSetShader(This, pPixelShader, ppClassInstances, NumClassInstances);
	if (gStereoTextureLeft > 0) {
		if (gl_left)
			This->PSSetShaderResources(125, 1, &gStereoResourceViewLeft);
		else
			This->PSSetShaderResources(125, 1, &gStereoResourceViewRight);
	}
	if (gIniTexture > 0)
		This->PSSetShaderResources(120, 1, &gIniResourceView);
}

void STDMETHODCALLTYPE D3D11C_CSSetShader(ID3D11DeviceContext * This, ID3D11ComputeShader *pComputeShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sCSSetShader_Hook.fnCSSetShader(This, pComputeShader, ppClassInstances, NumClassInstances);
	if (gStereoTextureLeft > 0) {
		if (gl_left)
			This->CSSetShaderResources(125, 1, &gStereoResourceViewLeft);
		else
			This->CSSetShaderResources(125, 1, &gStereoResourceViewRight);
	}
	if (gIniTexture > 0)
		This->CSSetShaderResources(120, 1, &gIniResourceView);
}

void STDMETHODCALLTYPE D3D11C_GSSetShader(ID3D11DeviceContext * This, ID3D11GeometryShader *pGeometryShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sGSSetShader_Hook.fnGSSetShader(This, pGeometryShader, ppClassInstances, NumClassInstances);
	if (gStereoTextureLeft > 0) {
		if (gl_left)
			This->GSSetShaderResources(125, 1, &gStereoResourceViewLeft);
		else
			This->GSSetShaderResources(125, 1, &gStereoResourceViewRight);
	}
	if (gIniTexture > 0)
		This->GSSetShaderResources(120, 1, &gIniResourceView);
}

void STDMETHODCALLTYPE D3D11C_HSSetShader(ID3D11DeviceContext * This, ID3D11HullShader *pHullShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sHSSetShader_Hook.fnHSSetShader(This, pHullShader, ppClassInstances, NumClassInstances);
	if (gStereoTextureLeft > 0) {
		if (gl_left)
			This->HSSetShaderResources(125, 1, &gStereoResourceViewLeft);
		else
			This->HSSetShaderResources(125, 1, &gStereoResourceViewRight);
	}
	if (gIniTexture > 0)
		This->HSSetShaderResources(120, 1, &gIniResourceView);
}

void STDMETHODCALLTYPE D3D11C_DSSetShader(ID3D11DeviceContext * This, ID3D11DomainShader *pDomainShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
	sDSSetShader_Hook.fnDSSetShader(This, pDomainShader, ppClassInstances, NumClassInstances);
	if (gStereoTextureLeft > 0) {
		if (gl_left)
			This->DSSetShaderResources(125, 1, &gStereoResourceViewLeft);
		else
			This->DSSetShaderResources(125, 1, &gStereoResourceViewRight);
	}
	if (gIniTexture > 0)
		This->DSSetShaderResources(120, 1, &gIniResourceView);
}
#pragma endregion

HRESULT STDMETHODCALLTYPE DXGIH_Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags) {
	gl_left = !gl_left;
	return sDXGI_Present_Hook.fnDXGI_Present(This, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DXGI_CreateSwapChain1(IDXGIFactory1* This, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
	LogInfo("CreateSwapChain1\n");
	HRESULT hr = sCreateSwapChain_Hook.fnCreateSwapChain1(This, pDevice, pDesc, ppSwapChain);
	if (!gl_Present_hooked) {
		LogInfo("Present hooked\n");
		gl_Present_hooked = true;
		DWORD_PTR*** vTable = (DWORD_PTR***)*ppSwapChain;
		DXGI_Present origPresent = (DXGI_Present)(*vTable)[8];
		cHookMgr.Hook(&(sDXGI_Present_Hook.nHookId), (LPVOID*)&(sDXGI_Present_Hook.fnDXGI_Present), origPresent, DXGIH_Present);
	}
	return hr;
}

void HackedPresent() {
	IDXGIFactory1* pFactory;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)(&pFactory));
	DWORD_PTR*** vTable = (DWORD_PTR***)pFactory;
	DXGI_CSC1 origCSC1 = (DXGI_CSC1)(*vTable)[10];
	cHookMgr.Hook(&(sCreateSwapChain_Hook.nHookId), (LPVOID*)&(sCreateSwapChain_Hook.fnCreateSwapChain1), origCSC1, DXGI_CreateSwapChain1);
	pFactory->Release();
}

#pragma region Hooks
HRESULT CreateStereoParamTextureAndView(ID3D11Device* d3d11)
{
	HRESULT hr = 0;

	float eyeSep = gEyeDist / (2.54f * gScreenSize * 16 / sqrtf(256 + 81));

	const int StereoBytesPerPixel = 16;
	const int stagingWidth = 8;
	const int stagingHeight = 1;

	D3D11_SUBRESOURCE_DATA sysData;
	sysData.SysMemPitch = StereoBytesPerPixel * stagingWidth;
	sysData.pSysMem = new unsigned char[sysData.SysMemPitch * stagingHeight];
	float* leftEye = (float*)sysData.pSysMem;
	gFinalSep = eyeSep * gSep * 0.01f;
	leftEye[0] = -gFinalSep;
	leftEye[1] = gConv;
	leftEye[2] = 1.0f;

	D3D11_TEXTURE2D_DESC desc;
	desc.Width = stagingWidth;
	desc.Height = stagingHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = 0;
	d3d11->CreateTexture2D(&desc, &sysData, &gStereoTextureLeft);
	LogInfo("StereoTexture: %d\n", gStereoTextureLeft > 0);

	float* rightEye = (float*)sysData.pSysMem;
	rightEye[0] = gFinalSep;
	rightEye[1] = gConv;
	rightEye[2] = -1.0f;

	d3d11->CreateTexture2D(&desc, &sysData, &gStereoTextureRight);

	delete[] sysData.pSysMem;

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
	d3d11->CreateShaderResourceView(gStereoTextureLeft, &descRV, &gStereoResourceViewLeft);

	d3d11->CreateShaderResourceView(gStereoTextureRight, &descRV, &gStereoResourceViewRight);

	return S_OK;
}

void CreateINITexture(ID3D11Device* d3d11) {
	D3D11_TEXTURE1D_DESC desc;
	memset(&desc, 0, sizeof(D3D11_TEXTURE1D_DESC));
	D3D11_SUBRESOURCE_DATA initialData;
	initialData.pSysMem = &iniParams;
	initialData.SysMemPitch = sizeof(DirectX::XMFLOAT4) * INI_PARAMS_SIZE;

	desc.Width = 1;												// 1 texel, .rgba as a float4
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;	// float4
	desc.Usage = D3D11_USAGE_DYNAMIC;				// Read/Write access from GPU and CPU
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;		// As resource view, access via t120
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;		// allow CPU access for hotkeys
	desc.MiscFlags = 0;
	HRESULT ret = d3d11->CreateTexture1D(&desc, &initialData, &gIniTexture);
	LogInfo("IniTexture: %d\n", gIniTexture > 0);
	// Since we need to bind the texture to a shader input, we also need a resource view.
	// The pDesc is set to NULL so that it will simply use the desc format above.
	ret = d3d11->CreateShaderResourceView(gIniTexture, NULL, &gIniResourceView);
}

void InitializeStereo(ID3D11Device * pDevice) {
	CreateINITexture(pDevice);
	// Create our stereo parameter texture
	CreateStereoParamTextureAndView(pDevice);
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

			LogInfo("Context COM hooked\n");
		}
	}
}

void hook(ID3D11Device** ppDevice) {
	if (ppDevice != NULL && *ppDevice != NULL) {
		LogInfo("Hook device: %p\n", *ppDevice);
		if (!gl_hookedDevice) {
			DWORD_PTR*** vTable = (DWORD_PTR***)*ppDevice;

			D3D11_VS origVS = (D3D11_VS)(*vTable)[12];
			D3D11_GS origGS = (D3D11_GS)(*vTable)[13];
			D3D11_PS origPS = (D3D11_PS)(*vTable)[15];
			D3D11_HS origHS = (D3D11_HS)(*vTable)[16];
			D3D11_DS origDS = (D3D11_DS)(*vTable)[17];
			D3D11_CS origCS = (D3D11_CS)(*vTable)[18];

			D3D11_GIC origGIC = (D3D11_GIC)(*vTable)[40];

			cHookMgr.Hook(&(sCreateVertexShader_Hook.nHookId), (LPVOID*)&(sCreateVertexShader_Hook.fnCreateVertexShader), origVS, D3D11_CreateVertexShader);
			cHookMgr.Hook(&(sCreateGeometryShader_Hook.nHookId), (LPVOID*)&(sCreateGeometryShader_Hook.fnCreateGeometryShader), origGS, D3D11_CreateGeometryShader);
			cHookMgr.Hook(&(sCreatePixelShader_Hook.nHookId), (LPVOID*)&(sCreatePixelShader_Hook.fnCreatePixelShader), origPS, D3D11_CreatePixelShader);
			cHookMgr.Hook(&(sCreateHullShader_Hook.nHookId), (LPVOID*)&(sCreateHullShader_Hook.fnCreateHullShader), origHS, D3D11_CreateHullShader);
			cHookMgr.Hook(&(sCreateDomainShader_Hook.nHookId), (LPVOID*)&(sCreateDomainShader_Hook.fnCreateDomainShader), origDS, D3D11_CreateDomainShader);
			cHookMgr.Hook(&(sCreateComputeShader_Hook.nHookId), (LPVOID*)&(sCreateComputeShader_Hook.fnCreateComputeShader), origCS, D3D11_CreateComputeShader);
			
			cHookMgr.Hook(&(sGetImmediateContext_Hook.nHookId), (LPVOID*)&(sGetImmediateContext_Hook.fn), origGIC, D3D11_GetImmediateContext);
			LogInfo("Device COM hooked\n");

			HackedPresent();
			gl_hookedDevice = true;
		}
		InitializeStereo(*ppDevice);
	}
}
#pragma endregion

#pragma region exports
// Exported function (faking d3d11.dll's export)
// Exported function (faking d3d11.dll's export)
HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
	UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d11.dll"

	// Hooking IDirect3D Object from Original Library
	typedef HRESULT(WINAPI* D3D11_Type)(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels,
		UINT FeatureLevels, UINT SDKVersion, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);
	D3D11_Type D3D11CreateDevice_fn = (D3D11_Type)GetProcAddress(gl_hOriginalDll, "D3D11CreateDevice");
	HRESULT res = D3D11CreateDevice_fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext);
	if (res == S_OK) {
		hook(ppDevice);
	}
	return res;
}
HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
	const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext) {
	if (!gl_hOriginalDll) LoadOriginalDll(); // looking for the "right d3d11.dll"

	// Hooking IDirect3D Object from Original Library
	typedef HRESULT(WINAPI* D3D11_Type)(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, INT Flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels, UINT SDKVersion,
		const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc, IDXGISwapChain** ppSwapChain, ID3D11Device** ppDevice, D3D_FEATURE_LEVEL* pFeatureLevel, ID3D11DeviceContext** ppImmediateContext);
	D3D11_Type D3D11CreateDeviceAndSwapChain_fn = (D3D11_Type)GetProcAddress(gl_hOriginalDll, "D3D11CreateDeviceAndSwapChain");
	HRESULT res = D3D11CreateDeviceAndSwapChain_fn(pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
	if (res == S_OK) {
		hook(ppDevice);
	}
	return res;
}
#pragma endregion

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
	if (GetPrivateProfileString("Stereo", "StereoSeparation", "50", setting, MAX_PATH, iniFile)) {
		gSep = stof(setting);
	}
	if (GetPrivateProfileString("Stereo", "StereoConvergence", "1.0", setting, MAX_PATH, iniFile)) {
		gConv = stof(setting);
	}
	if (GetPrivateProfileString("Stereo", "EyeDistance", "6.3", setting, MAX_PATH, iniFile)) {
		gEyeDist = stof(setting);
	}
	if (GetPrivateProfileString("Stereo", "ScreenSize", "15.6", setting, MAX_PATH, iniFile)) {
		gScreenSize = stof(setting);
	}

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

	InitializeCriticalSection(&gl_CS);

	WIN32_FIND_DATA findFileData;
	HANDLE hFind;

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

void LoadOriginalDll(void) {
	wchar_t sysDir[MAX_PATH];
	::GetSystemDirectoryW(sysDir, MAX_PATH);
	wcscat_s(sysDir, MAX_PATH, L"\\D3D11.dll");
	if (!gl_hOriginalDll) gl_hOriginalDll = ::LoadLibraryExW(sysDir, NULL, NULL);
}

void ExitInstance() {    
	if (gl_hOriginalDll) {
		::FreeLibrary(gl_hOriginalDll);
	    gl_hOriginalDll = NULL;  
	}
}
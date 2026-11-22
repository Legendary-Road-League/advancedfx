// World-space logo renderer adapted from EASEND/advancedfx, commit e1b9d64 (MIT).
#include "stdafx.h"

#include "MirvAr.h"

#include "WrpConsole.h"
#include "../shared/AfxCommandLine.h"
extern advancedfx::CCommandLine* g_CommandLine;
#include "../shared/AfxConsole.h"
#include "../shared/StringTools.h"

#include <Windows.h>
#include <wincodec.h>
#include <d3dcompiler.h>
#include <SpoutDX.h>
#include <fstream>
#include <set>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "windowscodecs.lib")

CMirvAr g_MirvAr;

static const unsigned int c_MaxTextureSize = 2048;

template<class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// One receiver per visible sender, updated once on the render thread before recording draws.
class CMirvArFeed {
public:
    CMirvArFeed(ID3D11Device* device, const std::string& sender) {
        m_Receiver.OpenDirectX11(device);
        m_Receiver.SetReceiverName(sender.c_str());
    }
    ~CMirvArFeed() { SafeRelease(view); }
    void Update(ID3D11Device* device) {
        if (GetTickCount64() < m_RetryAt) return;
        ID3D11Texture2D* frame = m_Receiver.ReceiveTexture() ? m_Receiver.GetSenderTexture() : nullptr;
        if (!frame) m_RetryAt = GetTickCount64() + 500;
        if (frame == m_Frame && view) return;
        SafeRelease(view);
        m_Frame = frame;
        if (!frame) return;
        D3D11_TEXTURE2D_DESC desc;
        frame->GetDesc(&desc);
        width = desc.Width; height = desc.Height;
        device->CreateShaderResourceView(frame, nullptr, &view);
    }
    ID3D11ShaderResourceView* view = nullptr;
    unsigned int width = 0, height = 0;
private:
    spoutDX m_Receiver;
    ID3D11Texture2D* m_Frame = nullptr; // Owned by the SDK, retained by view.
    ULONGLONG m_RetryAt = 0;
};

// Drop unused receivers and refresh a shared GPU texture only once for all its placements.
void CMirvAr::RefreshFeeds(ID3D11Device* device) {
    std::set<std::string> active;
    if (m_Enabled) for (const auto& entry : m_Items) {
        const auto& item = entry.second;
        if (item.visible && !item.sender.empty()) active.insert(item.sender);
    }
    for (auto it = m_Feeds.begin(); it != m_Feeds.end();) {
        if (!active.count(it->first)) it = m_Feeds.erase(it);
        else ++it;
    }
    for (const auto& sender : active) {
        auto& feed = m_Feeds[sender];
        if (!feed) feed = std::make_shared<CMirvArFeed>(device, sender);
        feed->Update(device);
    }
}

// Supply the static mip chain or the current shared frame, preserving its aspect ratio.
ID3D11ShaderResourceView* CMirvAr::TextureFor(ID3D11Device* device, const std::string& id, Item& item) {
    if (item.sender.empty()) return EnsureTexture(device, id, item) ? item.srv : nullptr;
    const auto it = m_Feeds.find(item.sender);
    if (it == m_Feeds.end() || !it->second->view) return nullptr;
    item.w = it->second->width;
    item.h = it->second->height;
    return it->second->view;
}

// Use the same parsed launch flag as the HLAE observer runtime.
bool CMirvAr::IsInsecure() {
	// Never do anything in a VAC secured session.
	return g_CommandLine && g_CommandLine->FindParam(L"-insecure");
}

// Schedule a render pass only for a pending measurement or a visible image.
bool CMirvAr::HasVisible() {
	std::unique_lock<std::mutex> lock(m_Mutex);
	if (m_PickPending || !m_Feeds.empty()) return true;
	if (!m_Enabled) return false;
	for (auto& it : m_Items) if (it.second.visible && (!it.second.mips.empty() || !it.second.sender.empty())) return true;
	return false;
}

// Keep decoded pixels for device recreation and release this logo's GPU resources.
void CMirvAr::ReleaseItemDevice(Item& item) {
	SafeRelease(item.srv);
	SafeRelease(item.tex);
}

// Release pipeline state and allow shaders to initialize on the next device.
void CMirvAr::ReleaseOwnDeviceObjects() {
	SafeRelease(m_Vs);
	SafeRelease(m_Ps);
	SafeRelease(m_Layout);
	SafeRelease(m_Vb);
	SafeRelease(m_PsCb);
	SafeRelease(m_Sampler);
	for (auto& b : m_Blend) SafeRelease(b);
	SafeRelease(m_Depth);
	SafeRelease(m_Raster);
	SafeRelease(m_PickCs);
	SafeRelease(m_PickCsMs);
	m_Dev = nullptr;
	m_ShadersTried = false;
}

// Serialize graphics-device teardown with changes made by console commands.
void CMirvAr::ReleaseDeviceObjects() {
	std::unique_lock<std::mutex> lock(m_Mutex);
    m_Feeds.clear();
	for (auto& it : m_Items) ReleaseItemDevice(it.second);
	ReleaseOwnDeviceObjects();
}

// ---------------------------------------------------------------------------
// Shaders, compiled at runtime (d3dcompiler_47.dll ships with Windows 10+).

static const char c_ArVs[] = R"(
cbuffer VS_CONSTANT_BUFFER : register(b0)
{
	float4x4 cViewProj;
};
struct VI { float3 p : POSITION; float2 t : TEXCOORD0; };
struct VO { float4 p : SV_POSITION; float2 t : TEXCOORD0; };
VO main(VI i)
{
	VO o;
	// Keep clip space: the GPU clips at the near plane and interpolates
	// the texture perspective-correct.
	o.p = mul(float4(i.p, 1), cViewProj);
	o.t = i.t;
	return o;
}
)";

static const char c_ArPs[] = R"(
Texture2D texture0 : register(t0);
SamplerState sampler0 : register(s0);
cbuffer PS_AR : register(b0)
{
	float4 cParams; // x opacity, y brightness, z blend mode
};
struct PI { float4 p : SV_POSITION; float2 t : TEXCOORD0; };
float4 main(PI i) : SV_TARGET
{
	float4 c = texture0.Sample(sampler0, i.t);
	float a = c.a * cParams.x;
	clip(a - 0.004);
	float3 rgb = saturate(c.rgb * cParams.y);
	int mode = (int)(cParams.z + 0.5);
	if (mode == 1) return float4(lerp(float3(1, 1, 1), rgb, a), 1); // multiply: dst * out
	if (mode == 2 || mode == 3) return float4(rgb * a, 1);          // screen / add
	return float4(rgb, a);                                           // normal
}
)";

static const char c_PickCs[] = R"(
Texture2D<float> depthTex : register(t0);
RWStructuredBuffer<float> outBuf : register(u0);
cbuffer PICK : register(b0) { int4 p0; int4 p1; int4 p2; };
[numthreads(1, 1, 1)]
void main()
{
	outBuf[0] = depthTex.Load(int3(p0.xy, 0));
	outBuf[1] = depthTex.Load(int3(p1.xy, 0));
	outBuf[2] = depthTex.Load(int3(p2.xy, 0));
}
)";

static const char c_PickCsMs[] = R"(
Texture2DMS<float> depthTex : register(t0);
RWStructuredBuffer<float> outBuf : register(u0);
cbuffer PICK : register(b0) { int4 p0; int4 p1; int4 p2; };
[numthreads(1, 1, 1)]
void main()
{
	outBuf[0] = depthTex.Load(p0.xy, 0);
	outBuf[1] = depthTex.Load(p1.xy, 0);
	outBuf[2] = depthTex.Load(p2.xy, 0);
}
)";

typedef HRESULT(WINAPI* D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

// Resolve the Windows shader compiler once for this renderer.
static D3DCompile_t GetD3DCompile() {
	static D3DCompile_t fn = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		HMODULE h = LoadLibraryW(L"d3dcompiler_47.dll");
		if (h) fn = (D3DCompile_t)GetProcAddress(h, "D3DCompile");
	}
	return fn;
}

// Compile one shader entry point and report graphics-driver diagnostics.
static ID3DBlob* CompileShader(const char* source, const char* profile) {
	D3DCompile_t compile = GetD3DCompile();
	if (nullptr == compile) return nullptr;
	ID3DBlob* code = nullptr;
	ID3DBlob* errors = nullptr;
	HRESULT hr = compile(source, strlen(source), "mirv_ar", nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (FAILED(hr) || nullptr == code) {
		if (errors) advancedfx::Warning("mirv_ar: shader %s: %s\n", profile, (const char*)errors->GetBufferPointer());
		SafeRelease(errors);
		SafeRelease(code);
		return nullptr;
	}
	SafeRelease(errors);
	return code;
}

struct ArVertex { float x, y, z, u, v; };

// Create and cache the depth-tested quad pipeline for the current D3D11 device.
bool CMirvAr::EnsureDeviceObjects(ID3D11Device* device) {
	if (nullptr == device) return false;
	if (m_Dev != device) {
		if (m_Dev) ReleaseOwnDeviceObjects();
		m_Dev = device;
	}
	if (m_Vs && m_Ps && m_Layout && m_Vb && m_PsCb && m_Sampler && m_Blend[3] && m_Depth && m_Raster) return true;
	if (m_ShadersTried) return false;
	m_ShadersTried = true;

	ID3DBlob* vs = CompileShader(c_ArVs, "vs_5_0");
	ID3DBlob* ps = CompileShader(c_ArPs, "ps_5_0");
	bool ok = vs && ps;
	if (ok) ok = SUCCEEDED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_Vs));
	if (ok) ok = SUCCEEDED(device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_Ps));
	if (ok) {
		D3D11_INPUT_ELEMENT_DESC desc[2] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
		};
		ok = SUCCEEDED(device->CreateInputLayout(desc, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &m_Layout));
	}
	SafeRelease(vs);
	SafeRelease(ps);
	if (ok) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(ArVertex) * 4;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ok = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &m_Vb));
	}
	if (ok) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 16;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		ok = SUCCEEDED(device->CreateBuffer(&bd, nullptr, &m_PsCb));
	}
	if (ok) {
		// anisotropic: logos stay sharp at steep angles
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_ANISOTROPIC;
		sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxAnisotropy = 8;
		sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
		sd.MinLOD = 0;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		ok = SUCCEEDED(device->CreateSamplerState(&sd, &m_Sampler));
	}
	if (ok) {
		const D3D11_BLEND src[4] = { D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ONE };
		const D3D11_BLEND dst[4] = { D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_ONE };
		for (int i = 0; ok && i < 4; ++i) {
			D3D11_BLEND_DESC bd = {};
			bd.RenderTarget[0].BlendEnable = TRUE;
			bd.RenderTarget[0].SrcBlend = src[i];
			bd.RenderTarget[0].DestBlend = dst[i];
			bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
			bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
			bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			ok = SUCCEEDED(device->CreateBlendState(&bd, &m_Blend[i]));
		}
	}
	if (ok) {
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = TRUE;
		dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		dd.StencilEnable = FALSE;
		ok = SUCCEEDED(device->CreateDepthStencilState(&dd, &m_Depth));
	}
	if (ok) {
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		ok = SUCCEEDED(device->CreateRasterizerState(&rd, &m_Raster));
	}
	if (!ok) {
		advancedfx::Warning("mirv_ar: own shaders not available, using the basic renderer.\n");
		bool tried = m_ShadersTried;
		ReleaseOwnDeviceObjects();
		m_Dev = device;
		m_ShadersTried = tried;
	}
	return ok;
}

// Upload the decoded mip chain once per logo and device.
bool CMirvAr::EnsureTexture(ID3D11Device* device, const std::string& id, Item& item) {
	if (item.srv) return true;
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = item.w;
	desc.Height = item.h;
	desc.MipLevels = (UINT)item.mips.size();
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_IMMUTABLE;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	std::vector<D3D11_SUBRESOURCE_DATA> init(item.mips.size());
	for (size_t i = 0; i < item.mips.size(); ++i) {
		init[i].pSysMem = item.mips[i].data();
		init[i].SysMemPitch = item.mipW[i] * 4;
		init[i].SysMemSlicePitch = 0;
	}
	if (FAILED(device->CreateTexture2D(&desc, init.data(), &item.tex)) || nullptr == item.tex) {
		advancedfx::Warning("mirv_ar: could not create texture for \"%s\".\n", id.c_str());
		item.visible = false;
		return false;
	}
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = desc.Format;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MostDetailedMip = 0;
	sd.Texture2D.MipLevels = desc.MipLevels;
	if (FAILED(device->CreateShaderResourceView(item.tex, &sd, &item.srv)) || nullptr == item.srv) {
		ReleaseItemDevice(item);
		item.visible = false;
		return false;
	}
	return true;
}

// Source engine AngleVectors.
// Convert Source pitch, yaw and roll to a local orthonormal frame.
static void AngleVectors(const float ang[3], float f[3], float r[3], float u[3]) {
	const double d = 3.14159265358979323846 / 180.0;
	double sp = std::sin(ang[0] * d), cp = std::cos(ang[0] * d);
	double sy = std::sin(ang[1] * d), cy = std::cos(ang[1] * d);
	double sr = std::sin(ang[2] * d), cr = std::cos(ang[2] * d);
	f[0] = (float)(cp * cy); f[1] = (float)(cp * sy); f[2] = (float)(-sp);
	r[0] = (float)(-sr * sp * cy + cr * sy); r[1] = (float)(-sr * sp * sy - cr * cy); r[2] = (float)(-sr * cp);
	u[0] = (float)(cr * sp * cy + sr * sy); u[1] = (float)(cr * sp * sy - sr * cy); u[2] = (float)(cr * cp);
}

// Derive the four world-space corners from physical width and image aspect ratio.
void CMirvAr::Corners(const Item& item, float corners[4][3]) {
	float f[3], r[3], u[3];
	AngleVectors(item.ang, f, r, u);
	// The logo faces f, so for a viewer looking at it its right is -r.
	float hw = item.width * 0.5f;
	float hh = item.width * 0.5f * (float)item.h / (float)(item.w ? item.w : 1);
	for (int k = 0; k < 3; ++k) {
		corners[0][k] = item.pos[k] + r[k] * hw + u[k] * hh; // top-left
		corners[1][k] = item.pos[k] + r[k] * hw - u[k] * hh; // bottom-left
		corners[2][k] = item.pos[k] - r[k] * hw + u[k] * hh; // top-right
		corners[3][k] = item.pos[k] - r[k] * hw - u[k] * hh; // bottom-right
	}
}

// Render the visible logos into the observation frame without writing scene depth.
bool CMirvAr::Render(ID3D11Device* device, ID3D11DeviceContext* ctx) {
	std::unique_lock<std::mutex> lock(m_Mutex);
	if (nullptr == device || nullptr == ctx) return true;
    RefreshFeeds(device);
	if (!m_Enabled) return true;
	bool any = false;
	for (auto& it : m_Items) if (it.second.visible) { any = true; break; }
	if (!any) return true;
	if (!EnsureDeviceObjects(device)) return false;

	UINT stride = sizeof(ArVertex), offset = 0;
	ctx->IASetInputLayout(m_Layout);
	ctx->IASetVertexBuffers(0, 1, &m_Vb, &stride, &offset);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->VSSetShader(m_Vs, nullptr, 0);
	ctx->PSSetShader(m_Ps, nullptr, 0);
	ctx->PSSetSamplers(0, 1, &m_Sampler);
	ctx->PSSetConstantBuffers(0, 1, &m_PsCb);
	ctx->OMSetDepthStencilState(m_Depth, 0);
	ctx->RSSetState(m_Raster);

	for (auto& it : m_Items) {
		Item& item = it.second;
		if (!item.visible) continue;
        auto* view = TextureFor(device, it.first, item);
        if (!view) continue;
		if (item.opacity <= 0.004f) continue;

		float c[4][3];
		Corners(item, c);
		D3D11_MAPPED_SUBRESOURCE m;
		if (FAILED(ctx->Map(m_Vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
		ArVertex* v = (ArVertex*)m.pData;
		const float uv[4][2] = { {0, 0}, {0, 1}, {1, 0}, {1, 1} };
		for (int i = 0; i < 4; ++i) { v[i].x = c[i][0]; v[i].y = c[i][1]; v[i].z = c[i][2]; v[i].u = uv[i][0]; v[i].v = uv[i][1]; }
		ctx->Unmap(m_Vb, 0);

		if (SUCCEEDED(ctx->Map(m_PsCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
			float* p = (float*)m.pData;
			p[0] = item.opacity; p[1] = item.brightness; p[2] = (float)item.blend; p[3] = 0;
			ctx->Unmap(m_PsCb, 0);
		}
		int b = item.blend < 0 || item.blend > 3 ? 0 : item.blend;
		ctx->OMSetBlendState(m_Blend[b], nullptr, 0xffffffff);
		ctx->PSSetShaderResources(0, 1, &view);
		ctx->Draw(4, 0);
	}
	ID3D11ShaderResourceView* nullSrv = nullptr;
	ctx->PSSetShaderResources(0, 1, &nullSrv);
	return true;
}

// Provide textures and corners to the existing quad renderer if own shaders fail.
void CMirvAr::ForEachVisible(ID3D11Device* device, const std::function<void(ID3D11ShaderResourceView* srv, const float corners[4][3])>& fn) {
	std::unique_lock<std::mutex> lock(m_Mutex);
	if (!m_Enabled || nullptr == device) return;
	for (auto& it : m_Items) {
		Item& item = it.second;
		if (!item.visible) continue;
        auto* view = TextureFor(device, it.first, item);
        if (!view) continue;
		float corners[4][3];
		Corners(item, corners);
		fn(view, corners);
	}
}

// ---------------------------------------------------------------------------
// Pick: depth under the screen centre -> surface point and normal.

// Invert the frame projection matrix using pivoted elimination.
static bool Invert4(const double m[4][4], double out[4][4]) {
	double a[4][8];
	for (int r = 0; r < 4; ++r) for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.0 : 0.0);
	for (int c = 0; c < 4; ++c) {
		int piv = c;
		for (int r = c + 1; r < 4; ++r) if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
		if (std::fabs(a[piv][c]) < 1e-12) return false;
		if (piv != c) for (int k = 0; k < 8; ++k) std::swap(a[piv][k], a[c][k]);
		double d = a[c][c];
		for (int k = 0; k < 8; ++k) a[c][k] /= d;
		for (int r = 0; r < 4; ++r) if (r != c) {
			double f = a[r][c];
			if (f != 0) for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
		}
	}
	for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out[r][c] = a[r][c + 4];
	return true;
}

// Convert a clip-space sample to world coordinates using its homogeneous divisor.
static bool Unproject(const double inv[4][4], double x, double y, double z, double out[3]) {
	double v[4] = { x, y, z, 1 };
	double o[4];
	for (int r = 0; r < 4; ++r) o[r] = inv[r][0] * v[0] + inv[r][1] * v[1] + inv[r][2] * v[2] + inv[r][3] * v[3];
	if (std::fabs(o[3]) < 1e-12) return false;
	for (int k = 0; k < 3; ++k) out[k] = o[k] / o[3];
	return true;
}

// Select a readable shader-resource format for each supported depth texture.
static DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f) {
	switch (f) {
	case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

// Sample three neighboring pixels, using a compute shader for multisampled depth.
bool CMirvAr::ReadDepth(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, const int px[3][2], float outDepth[3], std::string& error) {
	ID3D11Resource* res = nullptr;
	dsv->GetResource(&res);
	ID3D11Texture2D* tex = nullptr;
	if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
	SafeRelease(res);
	if (nullptr == tex) { error = "no depth texture"; return false; }
	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);
	for (int i = 0; i < 3; ++i) {
		if (px[i][0] < 0 || px[i][1] < 0 || (UINT)px[i][0] >= desc.Width || (UINT)px[i][1] >= desc.Height) { SafeRelease(tex); error = "pick outside of the depth buffer"; return false; }
	}
	bool ok = false;

	DXGI_FORMAT srvFormat = DepthSrvFormat(desc.Format);
	if ((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) && DXGI_FORMAT_UNKNOWN != srvFormat) {
		// Compute shader reads 3 samples - works with MSAA too.
		bool ms = desc.SampleDesc.Count > 1;
		ID3D11ComputeShader*& cs = ms ? m_PickCsMs : m_PickCs;
		if (nullptr == cs) {
			ID3DBlob* code = CompileShader(ms ? c_PickCsMs : c_PickCs, "cs_5_0");
			if (code) device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
			SafeRelease(code);
		}
		ID3D11ShaderResourceView* srv = nullptr;
		ID3D11Buffer* buf = nullptr;
		ID3D11UnorderedAccessView* uav = nullptr;
		ID3D11Buffer* staging = nullptr;
		ID3D11Buffer* cb = nullptr;
		if (cs) {
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = srvFormat;
			if (ms) sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
			else { sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1; }
			device->CreateShaderResourceView(tex, &sd, &srv);

			D3D11_BUFFER_DESC bd = {};
			bd.ByteWidth = 16;
			bd.Usage = D3D11_USAGE_DEFAULT;
			bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			bd.StructureByteStride = 4;
			device->CreateBuffer(&bd, nullptr, &buf);
			if (buf) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
				ud.Format = DXGI_FORMAT_UNKNOWN;
				ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				ud.Buffer.NumElements = 4;
				device->CreateUnorderedAccessView(buf, &ud, &uav);
			}
			D3D11_BUFFER_DESC sbd = {};
			sbd.ByteWidth = 16;
			sbd.Usage = D3D11_USAGE_STAGING;
			sbd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sbd.MiscFlags = 0;
			sbd.StructureByteStride = 0;
			device->CreateBuffer(&sbd, nullptr, &staging);

			int pts[12] = { px[0][0], px[0][1], 0, 0, px[1][0], px[1][1], 0, 0, px[2][0], px[2][1], 0, 0 };
			D3D11_BUFFER_DESC cbd = {};
			cbd.ByteWidth = sizeof(pts);
			cbd.Usage = D3D11_USAGE_IMMUTABLE;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			D3D11_SUBRESOURCE_DATA init = { pts, 0, 0 };
			device->CreateBuffer(&cbd, &init, &cb);
		}
		if (cs && srv && uav && staging && cb) {
			// The depth buffer may still be bound for output - unbind it while reading.
			ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
			ID3D11DepthStencilView* oldDsv = nullptr;
			ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &oldDsv);
			ID3D11ComputeShader* oldCs = nullptr;
			UINT oldCsN = 0;
			ctx->CSGetShader(&oldCs, nullptr, &oldCsN);
			ID3D11ShaderResourceView* oldSrv = nullptr;
			ctx->CSGetShaderResources(0, 1, &oldSrv);
			ID3D11UnorderedAccessView* oldUav = nullptr;
			ctx->CSGetUnorderedAccessViews(0, 1, &oldUav);
			ID3D11Buffer* oldCb = nullptr;
			ctx->CSGetConstantBuffers(0, 1, &oldCb);

			ctx->OMSetRenderTargets(0, nullptr, nullptr);
			ctx->CSSetShader(cs, nullptr, 0);
			ctx->CSSetShaderResources(0, 1, &srv);
			ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			ctx->CSSetConstantBuffers(0, 1, &cb);
			ctx->Dispatch(1, 1, 1);

			ID3D11ShaderResourceView* nullSrv = nullptr;
			ID3D11UnorderedAccessView* nullUav = nullptr;
			ctx->CSSetShaderResources(0, 1, &nullSrv);
			ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
			ctx->CopyResource(staging, buf);

			ctx->CSSetShader(oldCs, nullptr, 0);
			ctx->CSSetShaderResources(0, 1, &oldSrv);
			ctx->CSSetUnorderedAccessViews(0, 1, &oldUav, nullptr);
			ctx->CSSetConstantBuffers(0, 1, &oldCb);
			UINT n = 0;
			while (n < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT && rtvs[n]) ++n;
			ctx->OMSetRenderTargets(n, n ? rtvs : nullptr, oldDsv);
			for (auto& r : rtvs) SafeRelease(r);
			SafeRelease(oldDsv); SafeRelease(oldCs); SafeRelease(oldSrv); SafeRelease(oldUav); SafeRelease(oldCb);

			D3D11_MAPPED_SUBRESOURCE m;
			if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
				const float* f = (const float*)m.pData;
				for (int i = 0; i < 3; ++i) outDepth[i] = f[i];
				ctx->Unmap(staging, 0);
				ok = true;
			}
		}
		SafeRelease(srv); SafeRelease(uav); SafeRelease(buf); SafeRelease(staging); SafeRelease(cb);
		if (!ok) error = "could not read depth (shader)";
	}
	else if (desc.SampleDesc.Count <= 1) {
		// Copy the whole depth buffer to a CPU readable texture (once per pick).
		D3D11_TEXTURE2D_DESC sd = desc;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		sd.MipLevels = 1;
		sd.ArraySize = 1;
		ID3D11Texture2D* staging = nullptr;
		if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &staging)) && staging) {
			ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, tex, 0, nullptr);
			D3D11_MAPPED_SUBRESOURCE m;
			if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
				ok = true;
				for (int i = 0; i < 3; ++i) {
					const unsigned char* row = (const unsigned char*)m.pData + (size_t)px[i][1] * m.RowPitch;
					switch (desc.Format) {
					case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
						outDepth[i] = *(const float*)(row + (size_t)px[i][0] * 8); break;
					case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
						outDepth[i] = *(const float*)(row + (size_t)px[i][0] * 4); break;
					case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
						outDepth[i] = (float)((*(const unsigned int*)(row + (size_t)px[i][0] * 4)) & 0xFFFFFF) / 16777215.0f; break;
					case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM:
						outDepth[i] = (float)(*(const unsigned short*)(row + (size_t)px[i][0] * 2)) / 65535.0f; break;
					default:
						ok = false;
					}
				}
				ctx->Unmap(staging, 0);
				if (!ok) error = "unsupported depth format";
			}
			else error = "could not map depth copy";
		}
		else error = "could not copy depth";
		SafeRelease(staging);
	}
	else error = "depth buffer is multisampled and not readable - use two-view placement";

	SafeRelease(tex);
	return ok;
}

// Keep pick replies in memory so NetCon can read them on the engine thread.
void CMirvAr::FinishPick(const std::string& id, const std::string& result) {
	std::lock_guard<std::mutex> lock(m_Mutex);
	if (id == m_PickId) m_PickResult = result;
}

// Resolve the pending surface or ray query using the matrix and depth of this frame.
void CMirvAr::OnFrameDone(ID3D11Device* device, ID3D11DeviceContext* immediate, ID3D11DepthStencilView* dsv, const D3D11_VIEWPORT* vp, const double M[4][4]) {
	std::string id;
	bool ray;
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (!m_PickPending) return;
		m_PickPending = false;
		id = m_PickId;
		ray = m_PickRay;
	}
	char buf[512];
	auto fail = [&](const std::string& e) {
		std::string j = "{\"ok\":false,\"error\":\"";
		for (char c : e) if (c != '"' && c != '\\') j += c;
		j += "\"}";
		FinishPick(id, j);
	};
	if (nullptr == device || nullptr == immediate || nullptr == vp) return fail("no device");

	double inv[4][4];
	if (!Invert4(M, inv)) return fail("bad view matrix");

	// Two-view placement only needs the ray through the centre of the image.
	if (ray) {
		double origin[3], point[3];
		if (!Unproject(inv, 0, 0, 0, origin) || !Unproject(inv, 0, 0, 0.5, point))
			return fail("bad view matrix");
		snprintf(buf, sizeof(buf), "{\"ok\":true,\"origin\":[%.8f,%.8f,%.8f],\"point\":[%.8f,%.8f,%.8f]}",
			origin[0], origin[1], origin[2], point[0], point[1], point[2]);
		FinishPick(id, buf);
		return;
	}
	if (!dsv) return fail("no depth buffer this frame");
	const int d = (std::max)(2, (int)(vp->Width / 240.0f));
	const int cx = (int)(vp->TopLeftX + vp->Width * 0.5f);
	const int cy = (int)(vp->TopLeftY + vp->Height * 0.5f);
	const int px[3][2] = { { cx, cy }, { cx + d, cy }, { cx, cy - d } };
	float depth[3] = { 1, 1, 1 };
	std::string error;
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		ReadDepth(device, immediate, dsv, px, depth, error);
	}

	if (!error.empty()) return fail(error);
	const float dr = vp->MaxDepth - vp->MinDepth;
	double pts[3][3];
	for (int i = 0; i < 3; ++i) {
		if (depth[i] >= 0.999999f || depth[i] <= 0.0f) return fail(i == 0 ? "no surface under the crosshair (sky)" : "surface edge - aim at the middle of a wall or floor");
		double x = ((px[i][0] + 0.5) - vp->TopLeftX) / vp->Width * 2.0 - 1.0;
		double y = 1.0 - ((px[i][1] + 0.5) - vp->TopLeftY) / vp->Height * 2.0;
		double z = dr > 0 ? (depth[i] - vp->MinDepth) / dr : depth[i];
		if (!Unproject(inv, x, y, z, pts[i])) return fail("could not unproject");
	}
	double cam[3];
	if (!Unproject(inv, 0, 0, 0, cam)) return fail("could not unproject camera");

	double a[3], b[3], n[3];
	for (int k = 0; k < 3; ++k) { a[k] = pts[1][k] - pts[0][k]; b[k] = pts[2][k] - pts[0][k]; }
	n[0] = a[1] * b[2] - a[2] * b[1];
	n[1] = a[2] * b[0] - a[0] * b[2];
	n[2] = a[0] * b[1] - a[1] * b[0];
	double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (len < 1e-9) return fail("could not get the surface direction");
	for (auto& v : n) v /= len;
	double toCam[3] = { cam[0] - pts[0][0], cam[1] - pts[0][1], cam[2] - pts[0][2] };
	if (n[0] * toCam[0] + n[1] * toCam[1] + n[2] * toCam[2] < 0) for (auto& v : n) v = -v;
	double dist = std::sqrt(toCam[0] * toCam[0] + toCam[1] * toCam[1] + toCam[2] * toCam[2]);

	snprintf(buf, sizeof(buf), "{\"ok\":true,\"pos\":[%.3f,%.3f,%.3f],\"normal\":[%.5f,%.5f,%.5f],\"dist\":%.1f}",
		pts[0][0], pts[0][1], pts[0][2], n[0], n[1], n[2], dist);
	FinishPick(id, buf);
}

// Build alpha-weighted mip levels to keep transparent logos legible at a distance.
static void BuildMips(unsigned int w, unsigned int h, std::vector<unsigned char>&& level0,
	std::vector<std::vector<unsigned char>>& mips, std::vector<unsigned int>& mipW, std::vector<unsigned int>& mipH) {
	mips.clear(); mipW.clear(); mipH.clear();
	mips.emplace_back(std::move(level0)); mipW.push_back(w); mipH.push_back(h);
	while (w > 1 || h > 1) {
		unsigned int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
		const std::vector<unsigned char>& src = mips.back();
		std::vector<unsigned char> dst((size_t)nw * nh * 4);
		for (unsigned int y = 0; y < nh; ++y) for (unsigned int x = 0; x < nw; ++x) {
			unsigned int x0 = (std::min)(x * 2, w - 1), x1 = (std::min)(x * 2 + 1, w - 1);
			unsigned int y0 = (std::min)(y * 2, h - 1), y1 = (std::min)(y * 2 + 1, h - 1);
			const unsigned char* p[4] = { &src[(y0 * w + x0) * 4], &src[(y0 * w + x1) * 4], &src[(y1 * w + x0) * 4], &src[(y1 * w + x1) * 4] };
			// alpha-weighted average so transparent edges don't bleed dark colour
			unsigned int a = p[0][3] + p[1][3] + p[2][3] + p[3][3];
			unsigned char* o = &dst[(y * nw + x) * 4];
			for (int c = 0; c < 3; ++c) {
				unsigned int s = p[0][c] * p[0][3] + p[1][c] * p[1][3] + p[2][c] * p[2][3] + p[3][c] * p[3][3];
				o[c] = a ? (unsigned char)(s / a) : (unsigned char)((p[0][c] + p[1][c] + p[2][c] + p[3][c]) / 4);
			}
			o[3] = (unsigned char)(a / 4);
		}
		mips.emplace_back(std::move(dst)); mipW.push_back(nw); mipH.push_back(nh);
		w = nw; h = nh;
	}
}

// Decode and downsize artwork with WIC before handing pixels to the render thread.
bool CMirvAr::LoadImageFile(const std::string& utf8Path, Item& item, std::string& outError) {
	std::wstring wPath;
	if (!UTF8StringToWideString(utf8Path.c_str(), wPath)) { outError = "bad path encoding"; return false; }
    // The descriptor contains only a local sender name. Network URLs stay in the Manager.
    if (wPath.size() >= 6 && wPath.compare(wPath.size() - 6, 6, L".spout") == 0) {
        std::ifstream input(wPath.c_str(), std::ios::binary);
        char name[256];
        input.read(name, sizeof(name));
        const auto size = input.gcount();
        if (!input.eof() || size <= 0 || size >= sizeof(name)
            || memchr(name, 0, size) || memchr(name, '\n', size) || memchr(name, '\r', size)) {
            outError = "cannot read Spout sender"; return false;
        }
        item.sender.assign(name, static_cast<size_t>(size));
        return true;
    }

	HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	bool needUninit = SUCCEEDED(hrInit);

	IWICImagingFactory* factory = nullptr;
	IWICBitmapDecoder* decoder = nullptr;
	IWICBitmapFrameDecode* frame = nullptr;
	IWICBitmapScaler* scaler = nullptr;
	IWICFormatConverter* converter = nullptr;
	bool ok = false;

	do {
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) { outError = "WIC not available"; break; }
		if (FAILED(factory->CreateDecoderFromFilename(wPath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) { outError = "cannot open image"; break; }
		if (FAILED(decoder->GetFrame(0, &frame))) { outError = "cannot read image"; break; }
		UINT w = 0, h = 0;
		frame->GetSize(&w, &h);
		if (0 == w || 0 == h) { outError = "empty image"; break; }
		IWICBitmapSource* source = frame;
		if (w > c_MaxTextureSize || h > c_MaxTextureSize) {
			double k = (double)c_MaxTextureSize / (double)(w > h ? w : h);
			UINT nw = (UINT)(w * k), nh = (UINT)(h * k);
			if (nw < 1) nw = 1;
			if (nh < 1) nh = 1;
			if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame, nw, nh, WICBitmapInterpolationModeFant))) { outError = "cannot scale image"; break; }
			source = scaler;
			w = nw; h = nh;
		}
		if (FAILED(factory->CreateFormatConverter(&converter))) { outError = "cannot convert image"; break; }
		if (FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) { outError = "cannot convert image"; break; }
		std::vector<unsigned char> pixels((size_t)w * h * 4);
		if (FAILED(converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data()))) { outError = "cannot decode image"; break; }
		item.w = w;
		item.h = h;
		BuildMips(w, h, std::move(pixels), item.mips, item.mipW, item.mipH);
		ok = true;
	} while (false);

	if (converter) converter->Release();
	if (scaler) scaler->Release();
	if (frame) frame->Release();
	if (decoder) decoder->Release();
	if (factory) factory->Release();
	if (needUninit) CoUninitialize();
	return ok;
}

// Dispatch logo placement, appearance and measurement commands on the engine thread.
void CMirvAr::Console(int argc, const char* const* argv) {
	const char* arg0 = argc > 0 ? argv[0] : "mirv_ar";

	if (!IsInsecure()) {
		advancedfx::Warning("%s: disabled - CS2 is not running with -insecure.\n", arg0);
		return;
	}

	if (2 <= argc) {
		const char* cmd = argv[1];

		if (0 == _stricmp(cmd, "set") && 11 <= argc) {
			// set <id> <file> <x> <y> <z> <pitch> <yaw> <roll> <width> [visible]
			std::string id(argv[2]);
			std::string file(argv[3]);
			Item next;
			next.file = file;
			for (int k = 0; k < 3; ++k) next.pos[k] = (float)atof(argv[4 + k]);
			for (int k = 0; k < 3; ++k) next.ang[k] = (float)atof(argv[7 + k]);
			next.width = (float)atof(argv[10]);
			if (12 <= argc) next.visible = 0 != atoi(argv[11]);

			bool needLoad = true;
			{
				std::unique_lock<std::mutex> lock(m_Mutex);
				auto it = m_Items.find(id);
				if (it != m_Items.end() && it->second.file == file) {
					// same image - only move / resize, keep texture and style
					Item& cur = it->second;
					memcpy(cur.pos, next.pos, sizeof(cur.pos));
					memcpy(cur.ang, next.ang, sizeof(cur.ang));
					cur.width = next.width;
					cur.visible = next.visible;
					needLoad = false;
				}
			}
			if (needLoad) {
				std::string error;
				if (!LoadImageFile(file, next, error)) {
					advancedfx::Warning("%s: \"%s\": %s.\n", arg0, file.c_str(), error.c_str());
					return;
				}
				std::unique_lock<std::mutex> lock(m_Mutex);
				auto it = m_Items.find(id);
				if (it != m_Items.end()) {
					ReleaseItemDevice(it->second); // render thread recreates it
					next.opacity = it->second.opacity;
					next.brightness = it->second.brightness;
					next.blend = it->second.blend;
					it->second = std::move(next);
				} else {
					m_Items.emplace(id, std::move(next));
				}
			}
			return;
		}
		if (0 == _stricmp(cmd, "move") && 10 <= argc) {
			// move <id> <x> <y> <z> <pitch> <yaw> <roll> <width>
			std::unique_lock<std::mutex> lock(m_Mutex);
			auto it = m_Items.find(argv[2]);
			if (it == m_Items.end()) { advancedfx::Warning("%s: no item \"%s\".\n", arg0, argv[2]); return; }
			for (int k = 0; k < 3; ++k) it->second.pos[k] = (float)atof(argv[3 + k]);
			for (int k = 0; k < 3; ++k) it->second.ang[k] = (float)atof(argv[6 + k]);
			it->second.width = (float)atof(argv[9]);
			return;
		}
		if (0 == _stricmp(cmd, "style") && 6 <= argc) {
			// style <id> <opacity 0..1> <blend 0..3> <brightness 0..2>
			std::unique_lock<std::mutex> lock(m_Mutex);
			auto it = m_Items.find(argv[2]);
			if (it == m_Items.end()) { advancedfx::Warning("%s: no item \"%s\".\n", arg0, argv[2]); return; }
			it->second.opacity = (std::max)(0.0f, (std::min)(1.0f, (float)atof(argv[3])));
			it->second.blend = (std::max)(0, (std::min)(3, atoi(argv[4])));
			it->second.brightness = (std::max)(0.0f, (std::min)(2.0f, (float)atof(argv[5])));
			return;
		}
		// Advertise the supported protocol and correlate each asynchronous pick by ID.
		if (0 == _stricmp(cmd, "protocol")) {
            advancedfx::Message("LRL_MAP_ADS_LIVE 1\n");
			advancedfx::Message("LRL_MAP_ADS_READY 1\n");
			return;
		}
        if (0 == _stricmp(cmd, "livestatus")) {
            std::lock_guard<std::mutex> lock(m_Mutex);
            for (const auto& entry : m_Items) {
                if (entry.second.sender.empty() || !entry.second.visible) continue;
                const auto it = m_Feeds.find(entry.second.sender);
                advancedfx::Message("LRL_MAP_ADS_FEED %s %d\n", entry.first.c_str(),
                    it != m_Feeds.end() && it->second->view ? 1 : 0);
            }
            return;
        }
		if (argc >= 3 && (0 == _stricmp(cmd, "pick") || 0 == _stricmp(cmd, "result"))) {
			const std::string id(argv[2]);
			if (id.size() != 32 || id.find_first_not_of("0123456789abcdef") != std::string::npos) return;
			std::lock_guard<std::mutex> lock(m_Mutex);
			if (0 == _stricmp(cmd, "result")) {
				if (id == m_PickId && !m_PickResult.empty())
					advancedfx::Message("LRL_MAP_ADS_PICK %s %s\n", id.c_str(), m_PickResult.c_str());
			} else {
				m_PickId = id;
				m_PickRay = argc == 4 && 0 == _stricmp(argv[3], "ray");
				m_PickResult.clear();
				m_PickPending = true;
			}
			return;
		}
		if (0 == _stricmp(cmd, "visible") && 4 <= argc) {
			std::unique_lock<std::mutex> lock(m_Mutex);
			auto it = m_Items.find(argv[2]);
			if (it != m_Items.end()) it->second.visible = 0 != atoi(argv[3]);
			return;
		}
		if (0 == _stricmp(cmd, "remove") && 3 <= argc) {
			std::unique_lock<std::mutex> lock(m_Mutex);
			auto it = m_Items.find(argv[2]);
			if (it != m_Items.end()) {
				ReleaseItemDevice(it->second);
				m_Items.erase(it);
			}
			return;
		}
		// Manager map changes affect its own placements only.
		if (0 == _stricmp(cmd, "clear_lrl")) {
			std::lock_guard<std::mutex> lock(m_Mutex);
			for (auto it = m_Items.begin(); it != m_Items.end();) {
				if (it->first.compare(0, 4, "lrl_") != 0) { ++it; continue; }
				ReleaseItemDevice(it->second);
				it = m_Items.erase(it);
			}
			return;
		}
		if (0 == _stricmp(cmd, "clear")) {
			std::unique_lock<std::mutex> lock(m_Mutex);
			for (auto& it : m_Items) ReleaseItemDevice(it.second);
			m_Items.clear();
			return;
		}
		if (0 == _stricmp(cmd, "enabled")) {
			std::unique_lock<std::mutex> lock(m_Mutex);
			if (3 <= argc) m_Enabled = 0 != atoi(argv[2]);
			advancedfx::Message("%s enabled %d\n", arg0, m_Enabled ? 1 : 0);
			return;
		}
		if (0 == _stricmp(cmd, "list")) {
			std::unique_lock<std::mutex> lock(m_Mutex);
			for (auto& it : m_Items) {
				const Item& i = it.second;
				advancedfx::Message("%s: %s pos %.1f %.1f %.1f ang %.1f %.1f %.1f width %.1f %ux%u %s opacity %.2f blend %d brightness %.2f\n",
					it.first.c_str(), i.file.c_str(), i.pos[0], i.pos[1], i.pos[2], i.ang[0], i.ang[1], i.ang[2], i.width, i.w, i.h, i.visible ? "visible" : "hidden", i.opacity, i.blend, i.brightness);
			}
			advancedfx::Message("%u item(s), enabled %d\n", (unsigned)m_Items.size(), m_Enabled ? 1 : 0);
			return;
		}
	}

	advancedfx::Message(
		"%s set <id> <imageFile> <x> <y> <z> <pitch> <yaw> <roll> <width> [<visible 0|1>] - Add or replace a logo. It faces the direction of pitch/yaw (wall: pitch 0, floor: pitch -90), height follows the image aspect.\n"
		"%s move <id> <x> <y> <z> <pitch> <yaw> <roll> <width> - Move / resize a logo.\n"
		"%s style <id> <opacity 0..1> <blend 0 normal|1 multiply|2 screen|3 add> <brightness 0..2>\n"
		"%s pick <32-hex-id> [ray] - Sample the view centre. Read the JSON reply with mirv_ar result <id>.\n"
		"%s visible <id> 0|1\n"
		"%s remove <id>\n"
		"%s clear\n"
		"%s enabled [0|1]\n"
		"%s list\n"
		"Only works when CS2 runs with -insecure.\n",
		arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0, arg0
	);
}

CON_COMMAND(mirv_ar, "World-space sponsor logos for CS2 observer broadcasts.")
{
	int argc = args->ArgC();
	std::vector<const char*> argv;
	for (int i = 0; i < argc; ++i) argv.push_back(args->ArgV(i));
	g_MirvAr.Console(argc, argv.data());
}

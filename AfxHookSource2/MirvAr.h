#pragma once

// SHUD AR: textured quads (logos) drawn into the CS2 world, frame-accurate,
// depth-tested against the game's depth buffer. Controlled by the mirv_ar command.
//
// Own shaders (compiled at runtime with d3dcompiler_47): the vertex shader keeps
// clip space (no divide by w), so the GPU clips quads at the near plane and
// interpolates texture coordinates perspective-correct - no stretching when a
// logo is close to / behind the camera or seen at a steep angle.
// Per logo: opacity, brightness and blend mode (normal, multiply, screen, add).
// A request ID correlates centre-screen surface or ray samples with NetCon replies.

#include <d3d11.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CMirvArFeed;
class CMirvAr {
public:
	// Engine thread: is there anything to do this frame (draw or pick)?
	bool HasVisible();

	// Render thread, deferred context of the campath drawer, VS constant buffer b0
	// (cViewProj) already set. Returns false if the own shaders are not available,
	// then the caller may use fallbackFn (old path) for every visible item.
	bool Render(ID3D11Device* device, ID3D11DeviceContext* ctx);

	// Old path (shared drawtexture shaders) if Render() returned false.
	void ForEachVisible(ID3D11Device* device, const std::function<void(ID3D11ShaderResourceView* srv, const float corners[4][3])>& fn);

	// Render thread, after the frame's command list was executed: handles a
	// pending "pick". worldToScreen is the matrix used for this frame (row major,
	// clip = M * [x y z 1]).
	void OnFrameDone(ID3D11Device* device, ID3D11DeviceContext* immediate, ID3D11DepthStencilView* dsv, const D3D11_VIEWPORT* viewport, const double worldToScreen[4][4]);

	// Render thread: device is going away.
	void ReleaseDeviceObjects();

	void Console(int argc, const char* const* argv);

private:
	struct Item {
		std::string file;
        std::string sender; // Named Spout sender for live placements.
		float pos[3] = {0, 0, 0};
		float ang[3] = {0, 0, 0}; // pitch yaw roll (degrees), the logo faces the forward vector
		float width = 64;
		bool visible = true;
		float opacity = 1.0f;
		float brightness = 1.0f;
		int blend = 0; // 0 normal, 1 multiply, 2 screen, 3 add

		// decoded image (RGBA8, straight alpha), all mip levels
		unsigned int w = 0, h = 0;
		std::vector<std::vector<unsigned char>> mips;
		std::vector<unsigned int> mipW, mipH;

		ID3D11Texture2D* tex = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
	};

	std::mutex m_Mutex;
	std::map<std::string, Item> m_Items;
    // Render-thread receivers are shared by every visible placement of a sender.
    std::map<std::string, std::shared_ptr<CMirvArFeed>> m_Feeds;
    void RefreshFeeds(ID3D11Device* device);
    ID3D11ShaderResourceView* TextureFor(ID3D11Device* device, const std::string& id, Item& item);
	bool m_Enabled = true;
	std::string m_PickId, m_PickResult;
	bool m_PickPending = false, m_PickRay = false;
	void FinishPick(const std::string& id, const std::string& result);

	// device objects (render thread)
	ID3D11Device* m_Dev = nullptr;
	bool m_ShadersTried = false;
	ID3D11VertexShader* m_Vs = nullptr;
	ID3D11PixelShader* m_Ps = nullptr;
	ID3D11InputLayout* m_Layout = nullptr;
	ID3D11Buffer* m_Vb = nullptr;
	ID3D11Buffer* m_PsCb = nullptr;
	ID3D11SamplerState* m_Sampler = nullptr;
	ID3D11BlendState* m_Blend[4] = {nullptr, nullptr, nullptr, nullptr};
	ID3D11DepthStencilState* m_Depth = nullptr;
	ID3D11RasterizerState* m_Raster = nullptr;
	ID3D11ComputeShader* m_PickCs = nullptr;
	ID3D11ComputeShader* m_PickCsMs = nullptr;

	bool EnsureDeviceObjects(ID3D11Device* device);
	void ReleaseOwnDeviceObjects();
	bool EnsureTexture(ID3D11Device* device, const std::string& id, Item& item);
	void Corners(const Item& item, float corners[4][3]);
	bool ReadDepth(ID3D11Device* device, ID3D11DeviceContext* immediate, ID3D11DepthStencilView* dsv, const int px[3][2], float outDepth[3], std::string& error);

	static bool LoadImageFile(const std::string& utf8Path, Item& item, std::string& outError);
	static void ReleaseItemDevice(Item& item);
	static bool IsInsecure();
};

extern CMirvAr g_MirvAr;

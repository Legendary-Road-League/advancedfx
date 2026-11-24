#pragma once

#include <cstdint>
#include <functional>

class CEntityInstance;

// One immutable camera decision per game frame, shared by view setup and the DX11 pass queue.
namespace NadeCam {
    enum class View { Normal, Full, Pip };
    // Additional recording streams bypass the inset. Only the hidden pass captures its image.
    enum class Pass { Stream, Observer, Inset };
    struct Frame {
        View view = View::Normal;
        uint64_t shot = 0;
        float position[3]{}, angles[3]{};
        float fov = 90;
    };

    // Register the client mode variable after the game's console interface is available.
    void InitConsole();
    void OnAdd(CEntityInstance *entity, unsigned int handle);
    void OnRemove(unsigned int handle);
    void Reset();
    const Frame &CurrentFrame();
    // Rebuild the engine's view and visibility for the inset, then restore the same frame's observer.
    void RenderInset(const Frame &camera, const std::function<void()> &render);
}

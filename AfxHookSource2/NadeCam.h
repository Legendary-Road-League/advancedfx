#pragma once

#include <cstdint>

class CEntityInstance;

// One immutable camera decision per game frame, shared by view setup and the DX11 pass queue.
namespace NadeCam {
    enum class View { Normal, Full, PipObserver, PipGrenade };
    struct Frame {
        View view = View::Normal;
        uint64_t shot = 0;
        float position[3]{}, angles[3]{};
    };

    // Register the client mode variable after the game's console interface is available.
    void InitConsole();
    void OnAdd(CEntityInstance *entity, unsigned int handle);
    void OnRemove(unsigned int handle);
    void Reset();
    const Frame &CurrentFrame();
    void SetObserverImageReady(uint64_t shot);
}

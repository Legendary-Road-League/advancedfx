// CS2 spectator grenade camera. Entity handles and game time keep demo pauses and seeks deterministic.
#include "NadeCam.h"
#include "ClientEntitySystem.h"
#include "MirvTime.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>

extern SOURCESDK::CS2::ISource2EngineToClient *g_pEngineToClient;

namespace NadeCam {
namespace {
    enum class Mode { Off = 0, Pip = 1, Full = 2 };
    Mode mode = Mode::Off;
    SOURCESDK::CS2::Cvar_s *modeVar = nullptr;
    Frame frame;
    int lastFrame = -1;
    uint64_t shot = 0;
    std::atomic<uint64_t> observerImage{0};
    SOURCESDK::CS2::CBaseHandle target;
    float firstTime, lastMotion, lastTime, yaw;
    bool airborne = false;

    // Follow the actual recent flight path around bounces instead of extrapolating through walls.
    struct Sample { float time, x, y, z; };
    std::deque<Sample> trail;

    // Keep command aliases, native key bindings and an in-flight camera on the same mode.
    void ApplyMode(Mode selected) {
        if (selected == mode) return;
        mode = selected;
        if (mode == Mode::Off) Reset();
        else {
            ++shot;
            lastFrame = -1;
            frame.view = View::Normal;
        }
    }

    // Console writes can happen between an entity notification and the next render frame.
    void ReadMode() {
        if (modeVar) ApplyMode(static_cast<Mode>(modeVar->m_Value.m_i32Value));
    }

    // Resolve both parts of the handle so that reusing an entity slot cannot redirect a camera.
    CEntityInstance *Resolve(SOURCESDK::CS2::CBaseHandle handle) {
        if (!handle.IsValid() || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex) return nullptr;
        auto entity = static_cast<CEntityInstance *>(g_GetEntityFromIndex(*g_pEntityList, handle.GetEntryIndex()));
        return entity && entity->GetHandle() == handle ? entity : nullptr;
    }

    // Use only demo and spectator views, including a live CSTV observer.
    bool IsSpectating() {
        if (!g_pEngineToClient) return false;
        if (g_pEngineToClient->IsPlayingDemo()) return true;
        auto controller = GetLocalPlayerController();
        if (!controller || !controller->IsPlayerController()) return false;
        if (controller->GetTeam() == 1) return true;
        auto pawn = Resolve(controller->GetPlayerPawnHandle());
        return pawn && pawn->GetObserverMode() != 0;
    }

    // CS2 reports C_BaseGrenade for multiple projectiles. The designer name distinguishes them from weapons.
    bool IsGrenade(CEntityInstance *entity) {
        const char *name = entity->GetClassName();
        for (const char *type : {"hegrenade_projectile", "flashbang_projectile",
                "smokegrenade_projectile", "molotov_projectile", "decoy_projectile",
                "incgrenade_projectile"})
            if (0 == strcmp(name, type)) return true;
        return false;
    }
}

void InitConsole() {
    modeVar = WrpRegisterIntConVar("mirv_nadecam_mode", "Grenade camera: 0 off, 1 picture-in-picture, 2 fullscreen.", 0, 0, 2);
    if (!modeVar) advancedfx::Warning("Could not register mirv_nadecam_mode. Use mirv_nadecam off | pip | full.\n");
}

// Release the shot immediately on removal, map change, rewind, or manual cancellation.
void Reset() {
    target.Term();
    trail.clear();
    airborne = false;
    lastFrame = -1;
    frame = {};
}

// Lock the first new throw until it finishes, including when several players throw together.
void OnAdd(CEntityInstance *entity, unsigned int handle) {
    ReadMode();
    if (mode == Mode::Off || target.IsValid() || !entity || !IsGrenade(entity) || !IsSpectating()) return;
    target = SOURCESDK::CS2::CBaseHandle(handle);
    firstTime = lastMotion = lastTime = g_MirvTime.curtime_get();
    ++shot;
    lastFrame = -1;
}

// Entity lifecycle callbacks arrive on the game thread.
void OnRemove(unsigned int handle) {
    if (target.IsValid() && target.ToInt() == handle) Reset();
}

// A PiP shot must have a usable observer background before any grenade frames are rendered.
void SetObserverImageReady(uint64_t id) { observerImage.store(id); }

// Sample flight once per frame, preserving orientation through a pause and vertical bounces.
const Frame &CurrentFrame() {
    ReadMode();
    if (!target.IsValid()) return frame;
    const int count = g_MirvTime.framecount_get();
    if (count == lastFrame) return frame;
    lastFrame = count;
    const float now = g_MirvTime.curtime_get();
    auto entity = Resolve(target);
    if (!entity || !IsSpectating() || now < lastTime || now - lastTime > 1.0f || now - firstTime > 12.0f) {
        Reset();
        return frame;
    }
    lastTime = now;
    auto &offsets = g_clientDllOffsets;
    auto bytes = reinterpret_cast<const unsigned char *>(entity);
    if (offsets.C_BaseCSGrenadeProjectile.m_nExplodeEffectTickBegin &&
        *reinterpret_cast<const int *>(bytes + offsets.C_BaseCSGrenadeProjectile.m_nExplodeEffectTickBegin) > 0) {
        Reset();
        return frame;
    }
    if (!offsets.C_BaseEntity.m_pGameSceneNode || !offsets.CGameSceneNode.m_vecAbsOrigin ||
        !*reinterpret_cast<void *const *>(bytes + offsets.C_BaseEntity.m_pGameSceneNode)) return frame;
    Sample next{now};
    entity->GetOrigin(next.x, next.y, next.z);
    if (!std::isfinite(next.x) || !std::isfinite(next.y) || !std::isfinite(next.z)) {
        Reset();
        return frame;
    }
    if (!trail.empty()) {
        const auto &prev = trail.back();
        if (prev.x != next.x || prev.y != next.y || prev.z != next.z) lastMotion = now;
        if (prev.time == now) trail.pop_back();
    }
    trail.push_back(next);
    while (trail.size() > 1 && trail[1].time <= now - 0.085f) trail.pop_front();
    const auto &behind = trail.front();
    const float dx = next.x - behind.x, dy = next.y - behind.y, dz = next.z - behind.z;
    const float horizontal = std::hypot(dx, dy);
    if (horizontal > 0.5f) yaw = std::atan2(dy, dx) * 57.29578f;
    if (horizontal > 2 || std::abs(dz) > 2) airborne = true;
    // Smoke and decoy entities can outlive their flight for many seconds.
    if (now - lastMotion > 0.65f) { Reset(); return frame; }
    if (!airborne) return frame;
    frame.shot = shot;
    frame.position[0] = behind.x;
    frame.position[1] = behind.y;
    frame.position[2] = behind.z + 8;
    frame.angles[0] = std::atan2(8 - dz, horizontal) * 57.29578f;
    frame.angles[1] = yaw;
    if (mode == Mode::Full) frame.view = View::Full;
    else {
        const bool ready = observerImage.load() == shot;
        frame.view = ready && frame.view == View::PipObserver ? View::PipGrenade : View::PipObserver;
    }
    return frame;
}

// Console entry point shared by the two display modes and normal CS2 key bindings.
CON_COMMAND(mirv_nadecam, "Automatic CS2 spectator grenade camera: full, pip, off.") {
    ReadMode();
    if (args->ArgC() == 2) {
        const char *value = args->ArgV(1);
        Mode selected;
        if (!_stricmp(value, "full")) selected = Mode::Full;
        else if (!_stricmp(value, "pip")) selected = Mode::Pip;
        else if (!_stricmp(value, "off")) selected = Mode::Off;
        else { advancedfx::Warning("Usage: mirv_nadecam full | pip | off\n"); return; }
        if (modeVar && mode != selected) {
            // This client-only variable has no callbacks and is owned by the camera.
            modeVar->m_Value.m_i32Value = static_cast<int>(selected);
            ++modeVar->m_iTimesChanged;
        }
        ApplyMode(selected);
    }
    advancedfx::Message("mirv_nadecam: %s (%s). Usage: mirv_nadecam full | pip | off\n",
        mode == Mode::Off ? "off" : mode == Mode::Full ? "full" : "pip",
        target.IsValid() ? "tracking" : "idle");
}
}

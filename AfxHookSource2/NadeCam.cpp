// CS2 spectator grenade camera. Entity handles and game time keep demo pauses and seeks deterministic.
#include "NadeCam.h"
#include "ClientEntitySystem.h"
#include "MirvTime.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <optional>
#include <vector>

namespace NadeCam {
namespace {
    enum class Mode { Off = 0, Pip = 1, Full = 2 };
    Mode mode = Mode::Off;
    SOURCESDK::CS2::Cvar_s *modeVar = nullptr;
    SOURCESDK::CS2::Cvar_s *impactFovVar = nullptr;
    Frame frame;
    int lastFrame = -1;
    uint64_t shot = 0;
    using Handle = SOURCESDK::CS2::CBaseHandle;
    Handle target, observedThrower;
    float firstTime, lastMotion, lastTime, yaw;
    bool airburst = false;

    // Follow the actual recent flight path around bounces instead of extrapolating through walls.
    struct Sample { float time, x, y, z; };
    std::deque<Sample> trail;

    // Entity-added precedes network property initialization. Keep the observer at the time of creation.
    struct Candidate { Handle projectile, observer; float added; std::optional<Sample> first; };
    std::vector<Candidate> pending;

    // The effect outlives the projectile, so hold an independent view of its last world position.
    struct Impact { Sample center, from, wide; float start, startFov; };
    std::optional<Impact> impact;

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

    // In-eye and chase modes have a selected player. Roaming and fixed cameras have no thrower.
    Handle ObservedPawn() {
        auto controller = GetLocalPlayerController();
        if (!controller || !controller->IsPlayerController()) return {};
        auto pawn = Resolve(controller->GetPlayerPawnHandle());
        if (!pawn) return {};
        const int observerMode = pawn->GetObserverMode();
        if (observerMode != 2 && observerMode != 3) return {};
        const auto handle = pawn->GetObserverTarget();
        auto player = Resolve(handle);
        return player && player->IsPlayerPawn() ? handle : Handle{};
    }

    // Prefer the grenade's own thrower. Older schemas may expose only the base entity's owner.
    Handle Thrower(CEntityInstance *entity) {
        const auto bytes = reinterpret_cast<const unsigned char *>(entity);
        for (auto offset : {g_clientDllOffsets.C_BaseGrenade.m_hThrower, g_clientDllOffsets.C_BaseEntity.m_hOwnerEntity}) {
            if (!offset) continue;
            Handle handle(*reinterpret_cast<const unsigned int *>(bytes + offset));
            if (handle.IsValid()) return handle;
        }
        return {};
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

    // Ignore smoke already blooming and projectiles retained after the blast.
    bool HasDetonated(CEntityInstance *entity) {
        const auto bytes = reinterpret_cast<const unsigned char *>(entity);
        const auto blast = g_clientDllOffsets.C_BaseCSGrenadeProjectile.m_nExplodeEffectTickBegin;
        const auto smoke = g_clientDllOffsets.C_SmokeGrenadeProjectile.m_bDidSmokeEffect;
        return (blast && *reinterpret_cast<const int *>(bytes + blast) > 0) ||
            (smoke && !strcmp(entity->GetClassName(), "smokegrenade_projectile") && bytes[smoke]);
    }

    // Read a position only after CS2 has attached the networked scene node.
    std::optional<Sample> Position(CEntityInstance *entity, float now) {
        const auto offset = g_clientDllOffsets.C_BaseEntity.m_pGameSceneNode;
        if (!offset || !g_clientDllOffsets.CGameSceneNode.m_vecAbsOrigin ||
            !*reinterpret_cast<void *const *>(reinterpret_cast<const unsigned char *>(entity) + offset)) return {};
        Sample point{now};
        entity->GetOrigin(point.x, point.y, point.z);
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) return {};
        return point;
    }

    // Select a moving projectile belonging to the same pawn at throw time and at render time.
    void SelectThrow(Handle observer, float now) {
        for (auto it = pending.begin(); it != pending.end();) {
            auto entity = Resolve(it->projectile);
            if (!entity || it->observer != observer || now < it->added || now - it->added > 0.55f || HasDetonated(entity)) {
                it = pending.erase(it);
                continue;
            }
            const auto owner = Thrower(entity);
            if (owner.IsValid() && owner != observer) { it = pending.erase(it); continue; }
            const auto point = Position(entity, now);
            if (point && owner == observer) {
                if (!it->first) it->first = point;
                const auto &a = *it->first;
                if (std::hypot(std::hypot(point->x - a.x, point->y - a.y), point->z - a.z) > 2) {
                    target = it->projectile;
                    observedThrower = observer;
                    const char *name = entity->GetClassName();
                    airburst = !strcmp(name, "hegrenade_projectile") || !strcmp(name, "flashbang_projectile");
                    impact.reset();
                    trail.assign(1, a);
                    firstTime = lastMotion = lastTime = now;
                    ++shot;
                    pending.clear();
                    return;
                }
            }
            ++it;
        }
    }

    // Pull back along the approach to the impact. Prefer an elevated sample with a clear recent flight path.
    void BeginImpact(float now) {
        if (trail.empty()) return;
        const auto center = trail.back();
        const Sample from{now, frame.position[0], frame.position[1], frame.position[2]};
        const auto *wide = &center;
        float best = -1e9f;
        for (auto it = trail.rbegin(); it != trail.rend(); ++it) {
            const float distance = std::hypot(it->x - center.x, it->y - center.y);
            if (distance > 450) break;
            const float score = -std::abs(distance - 320) + std::clamp(it->z - center.z, -80.0f, 170.0f) * 0.6f;
            if (score > best) { best = score; wide = &*it; }
        }
        Sample end = *wide;
        // Low throws still need elevation to see the effect spread across the ground.
        end.z = (std::max)(end.z + 42, center.z + 150);
        impact = Impact{center, from, end, now, frame.fov};
        target.Term();
        trail.clear();
    }

    // Aim both output modes at the same world point. Keep yaw stable on almost vertical shots.
    void LookAt(const Sample &point) {
        const float dx = point.x - frame.position[0], dy = point.y - frame.position[1];
        const float horizontal = std::hypot(dx, dy);
        if (horizontal > 0.5f) yaw = std::atan2(dy, dx) * 57.29578f;
        frame.angles[0] = std::atan2(frame.position[2] - point.z, horizontal) * 57.29578f;
        frame.angles[1] = yaw;
    }

    // HE and flash effects are brief. Widen the lens and increase the chase distance before they burst.
    void ShowFlight(const Sample &point) {
        const float widen = airburst ? std::clamp((point.time - firstTime - 0.45f) / 0.65f, 0.0f, 1.0f) : 0;
        const float cameraTime = point.time - 0.085f - 0.25f * widen;
        const auto behind = std::lower_bound(trail.begin(), trail.end(), cameraTime,
            [](const Sample &sample, float time) { return sample.time < time; });
        frame.position[0] = behind->x;
        frame.position[1] = behind->y;
        frame.position[2] = behind->z + 8 + 32 * widen;
        frame.fov = 90 + 12 * widen;
        LookAt(point);
    }

    // Ease out over the first third of a second, then hold the actual smoke, fire or blast in frame.
    void ShowImpact(float now) {
        const auto &view = *impact;
        const float t = std::clamp((now - view.start) / 0.34f, 0.0f, 1.0f);
        const float blend = t * t * (3 - 2 * t);
        frame.position[0] = view.from.x + (view.wide.x - view.from.x) * blend;
        frame.position[1] = view.from.y + (view.wide.y - view.from.y) * blend;
        frame.position[2] = view.from.z + (view.wide.z - view.from.z) * blend;
        const float wideFov = impactFovVar ? static_cast<float>(impactFovVar->m_Value.m_i32Value) : 106.0f;
        frame.fov = view.startFov + (wideFov - view.startFov) * blend;
        LookAt(view.center);
    }
}

void InitConsole() {
    modeVar = WrpRegisterIntConVar("mirv_nadecam_mode", "Grenade camera: 0 off, 1 picture-in-picture, 2 fullscreen.", 0, 0, 2);
    // Only the landing lens changes; the observer's FOV and the inset dimensions stay independent.
    impactFovVar = WrpRegisterIntConVar("mirv_nadecam_impact_fov", "Landing camera field of view in degrees. Larger values show more of the surrounding area.", 106, 80, 120);
    if (!modeVar) advancedfx::Warning("Could not register mirv_nadecam_mode. Use mirv_nadecam off | pip | full.\n");
}

// Release the shot immediately on removal, map change, rewind, or manual cancellation.
void Reset() {
    target.Term();
    observedThrower.Term();
    impact.reset();
    pending.clear();
    trail.clear();
    lastFrame = -1;
    frame = {};
}

// Capture the selected player now, before network updates supply the projectile's owner and origin.
void OnAdd(CEntityInstance *entity, unsigned int handle) {
    ReadMode();
    if (mode == Mode::Off || !entity || !IsGrenade(entity)) return;
    const auto observer = ObservedPawn();
    if (observedThrower.IsValid() && observer != observedThrower) Reset();
    if (observer.IsValid() && !target.IsValid())
        pending.push_back({Handle(handle), observer, g_MirvTime.curtime_get(), {}});
}

// Entity lifecycle callbacks arrive on the game thread.
void OnRemove(unsigned int handle) {
    if (target.IsValid() && target.ToInt() == handle) BeginImpact(g_MirvTime.curtime_get());
}

// Sample flight once per frame, preserving orientation through a pause and vertical bounces.
const Frame &CurrentFrame() {
    ReadMode();
    if (!observedThrower.IsValid() && pending.empty()) return frame;
    const int count = g_MirvTime.framecount_get();
    if (count == lastFrame) return frame;
    lastFrame = count;
    const float now = g_MirvTime.curtime_get();
    const auto observer = ObservedPawn();
    if (!observer.IsValid() || (observedThrower.IsValid() && (observer != observedThrower ||
        now < lastTime || now - lastTime > 1.0f || now - firstTime > 14.0f))) {
        Reset();
        return frame;
    }
    if (!pending.empty()) SelectThrow(observer, now);
    if (!observedThrower.IsValid()) return frame;
    lastTime = now;
    if (!impact) {
        auto entity = Resolve(target);
        if (!entity) { Reset(); return frame; }
        const auto point = Position(entity, now);
        if (!point) { Reset(); return frame; }
        const auto &prev = trail.back();
        if (prev.x != point->x || prev.y != point->y || prev.z != point->z) lastMotion = now;
        if (prev.time == now) trail.pop_back();
        trail.push_back(*point);
        while (trail.size() > 1 && trail[1].time < now - 2.0f) trail.pop_front();
        if (HasDetonated(entity) || now - lastMotion > 0.22f) {
            BeginImpact(now);
        } else {
            ShowFlight(*point);
        }
    }
    if (impact) {
        if (now - impact->start >= 2.2f) { Reset(); return frame; }
        ShowImpact(now);
    }
    frame.shot = shot;
    frame.view = mode == Mode::Full ? View::Full : View::Pip;
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
        impact ? "impact" : target.IsValid() ? "tracking" : "idle");
}
}

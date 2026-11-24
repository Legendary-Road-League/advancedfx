// CS2 spectator grenade camera. Entity handles and game time keep demo pauses and seeks deterministic.
#include "NadeCam.h"
#include "ClientEntitySystem.h"
#include "MirvTime.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"
#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient *g_pEngineToClient;

namespace NadeCam {
namespace {
    enum class Mode { Off = 0, Pip = 1, Full = 2 };
    Mode mode = Mode::Off;
    SOURCESDK::CS2::Cvar_s *modeVar = nullptr;
    SOURCESDK::CS2::Cvar_s *impactFovVar = nullptr;
    Frame frame;
    int lastFrame = -1, lastDemoTick = -1;
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

    // CS2 clears the target in roaming mode. Keep both serialised handles until the return is complete.
    struct Observer { Handle spectator, player; int mode = 0; };
    enum class Transition { Entering, Roaming, AwaitingSpectator, Returning };
    struct FullFollow { Observer original; Handle controller; Transition state; ULONGLONG requested, retryAfter = 0; };
    std::optional<FullFollow> fullFollow;

    // Discard only projectile data while an already requested spectator transition settles.
    void ClearShot() {
        target.Term();
        observedThrower.Term();
        impact.reset();
        pending.clear();
        trail.clear();
        lastFrame = -1;
        lastDemoTick = -1;
        // Keep the last fullscreen image's camera until CS2 actually leaves roaming.
        if (!fullFollow || fullFollow->state != Transition::Returning) frame = {};
    }

    void ReturnToPlayer();

    // Keep command aliases, native key bindings and an in-flight camera on the same mode.
    void ApplyMode(Mode selected) {
        if (selected == mode) return;
        if (mode == Mode::Full) ReturnToPlayer();
        mode = selected;
        if (mode == Mode::Off) ClearShot();
        else {
            ++shot;
            lastFrame = -1;
            if (!fullFollow || fullFollow->state != Transition::Returning) frame.view = View::Normal;
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
    Observer ReadObserver() {
        auto controller = GetLocalPlayerController();
        if (!controller || !controller->IsPlayerController()) return {};
        auto pawn = Resolve(controller->GetPlayerPawnHandle());
        if (!pawn) return {};
        Observer view{pawn->GetHandle(), {}, pawn->GetObserverMode()};
        if (view.mode == 2 || view.mode == 3) {
            auto player = Resolve(pawn->GetObserverTarget());
            if (player && player->IsPlayerPawn()) view.player = player->GetHandle();
        }
        return view;
    }

    // Start only after a throw is selected. Native spectator commands take effect on a later frame.
    void EnterFreeLook(const Observer &view) {
        if (fullFollow || !g_pEngineToClient || view.player != observedThrower) return;
        auto player = Resolve(view.player);
        if (!player) return;
        const auto controller = player->GetPlayerControllerHandle();
        if (!Resolve(controller)) return;
        fullFollow = FullFollow{view, controller, Transition::Entering, GetTickCount64()};
        g_pEngineToClient->ExecuteClientCmd(0, "spec_mode 4", true);
    }

    // Restore the native mode first, then select the original controller by its numeric slot.
    // If the player left, CS2 can choose another target in that mode. A manual camera takes priority.
    void ReturnToPlayer() {
        if (!fullFollow) return;
        auto &follow = *fullFollow;
        const auto view = ReadObserver();
        // A demo seek temporarily removes the local spectator while rebuilding its entity list.
        // Wait for the same spectator to reappear before issuing commands against its new snapshot.
        if (!view.spectator.IsValid()) {
            if (follow.state != Transition::AwaitingSpectator) {
                follow.state = Transition::AwaitingSpectator;
                follow.requested = GetTickCount64();
            }
            return;
        }
        const auto now = GetTickCount64();
        if (follow.state == Transition::Returning && now < follow.retryAfter) return;
        const bool stillEntering = follow.state == Transition::Entering &&
            view.mode == follow.original.mode && view.player == follow.original.player;
        if (!g_pEngineToClient || view.spectator != follow.original.spectator || (view.mode != 4 && !stillEntering)) {
            fullFollow.reset();
            return;
        }
        std::string command = "spec_mode " + std::to_string(follow.original.mode);
        auto controller = Resolve(follow.controller);
        auto player = controller ? Resolve(controller->GetPlayerPawnHandle()) : nullptr;
        if (player && player->IsPlayerPawn() && player->GetPlayerControllerHandle() == follow.controller && player->GetHealth() > 0)
            command += ";spec_player " + std::to_string(follow.controller.GetEntryIndex());
        if (follow.state != Transition::Returning) follow.requested = now;
        follow.state = Transition::Returning;
        follow.retryAfter = now + 250;
        g_pEngineToClient->ExecuteClientCmd(0, command.c_str(), true);
    }

    // End the shot and release a free camera acquired for this throw.
    void FinishShot() {
        ReturnToPlayer();
        ClearShot();
    }

    // Only our own roaming transition inherits the thrower. Relinquish the camera on manual changes
    // or an unacknowledged command, including while a demo is paused.
    Handle TrackingPlayer(const Observer &view) {
        if (!fullFollow) return view.player;
        if (!view.spectator.IsValid()) {
            if (fullFollow->state == Transition::AwaitingSpectator && GetTickCount64() - fullFollow->requested >= 2000)
                Reset();
            else FinishShot();
            return {};
        }
        if (fullFollow->state == Transition::AwaitingSpectator) {
            ReturnToPlayer();
            return view.player;
        }
        auto &follow = *fullFollow;
        if (view.spectator == follow.original.spectator) {
            const bool pendingCommand = GetTickCount64() - follow.requested < 2000;
            if (view.mode == 4 && (follow.state != Transition::Returning || pendingCommand)) {
                const Handle player = follow.original.player;
                if (follow.state == Transition::Entering) follow.state = Transition::Roaming;
                // CS2 can discard a command during a seek. Retry briefly while we still own roaming.
                if (follow.state == Transition::Returning) ReturnToPlayer();
                return Resolve(player) ? player : Handle{};
            }
            if (view.mode == follow.original.mode && view.player == follow.original.player) {
                if (follow.state == Transition::Entering && pendingCommand) return view.player;
                if (follow.state == Transition::Returning) {
                    fullFollow.reset();
                    frame.view = View::Normal;
                    return view.player;
                }
            }
        }
        Reset();
        return view.player;
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

// A new level or a manual observer selection invalidates the old spectator session.
void Reset() {
    fullFollow.reset();
    ClearShot();
}

// Capture the selected player now, before network updates supply the projectile's owner and origin.
void OnAdd(CEntityInstance *entity, unsigned int handle) {
    ReadMode();
    if (mode == Mode::Off || !entity || !IsGrenade(entity)) return;
    const auto observer = TrackingPlayer(ReadObserver());
    if (observedThrower.IsValid() && observer != observedThrower) FinishShot();
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
    if (!observedThrower.IsValid() && pending.empty() && !fullFollow) return frame;
    const int count = g_MirvTime.framecount_get();
    if (count == lastFrame) return frame;
    lastFrame = count;
    float now = g_MirvTime.curtime_get();
    int tick = -1;
    const bool inDemo = g_MirvTime.GetCurrentDemoTick(tick) && tick >= 0;
    // Pausing or changing recording FPS can move interpolated client time back within a tick.
    // Use the replay tick to identify rewinds and keep flight samples ordered during ordinary playback.
    const bool rewound = inDemo ? lastDemoTick >= 0 && tick < lastDemoTick : now < lastTime;
    lastDemoTick = tick;
    const auto view = ReadObserver();
    const auto observer = TrackingPlayer(view);
    if (fullFollow && fullFollow->state == Transition::Returning && view.mode == 4) return frame;
    if (!observer.IsValid() || (observedThrower.IsValid() && (observer != observedThrower ||
        rewound || now - lastTime > 1.0f || now - firstTime > 14.0f))) {
        FinishShot();
        return frame;
    }
    if (observedThrower.IsValid()) now = (std::max)(lastTime, now);
    if (!pending.empty()) SelectThrow(observer, now);
    if (!observedThrower.IsValid()) return frame;
    if (mode == Mode::Full) EnterFreeLook(view);
    lastTime = now;
    if (!impact) {
        auto entity = Resolve(target);
        if (!entity) { FinishShot(); return frame; }
        const auto point = Position(entity, now);
        if (!point) { FinishShot(); return frame; }
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
        if (now - impact->start >= 2.2f) { FinishShot(); return frame; }
        ShowImpact(now);
    }
    frame.shot = shot;
    // Wait for the native roaming camera so the first fullscreen frame already has a free view.
    if (mode == Mode::Pip) frame.view = View::Pip;
    else frame.view = fullFollow && fullFollow->state == Transition::Roaming ? View::Full : View::Normal;
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

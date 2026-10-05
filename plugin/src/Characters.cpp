#include "Characters.h"

#include "Camera.h"
#include "Collision.h"
#include "Config.h"
#include "Hook.h"
#include "Input.h"
#include "Log.h"
#include "Mapping.h"
#include "Runtime.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace mp2;

Runtime& State()
{
    static Runtime runtime;
    return runtime;
}

namespace
{
    constexpr double kDegToRad = 0.017453292519943295;
    constexpr DWORD  kStaleMs = 2000;

    using UpdateFn = void(__thiscall*)(X_Character*, const X_TimeUpdate&);
    using PhysicsFn = void(__thiscall*)(X_Character*, float, P_Camera*);
    using DamageFn = void(__thiscall*)(X_Character*, float, float, int, X_Character*, const void*);

    UpdateFn  g_origPre = nullptr;
    UpdateFn  g_origPost = nullptr;
    PhysicsFn g_origPhysics = nullptr;
    DamageFn  g_origDamage = nullptr;
    using SetHealthFn = void(__thiscall*)(X_CharacterProperties*, float);
    using SetDeadFn = void(__thiscall*)(X_Character*, bool);
    using SetDyingFn = void(__thiscall*)(X_CharacterProperties*, bool);
    SetHealthFn g_origSetHealth = nullptr;
    SetDeadFn   g_origSetDead = nullptr;
    SetDyingFn  g_origSetDying = nullptr;

    struct Tracked
    {
        std::uint32_t id;
        DWORD         lastSeen;
        float         height;      // blocks (from the capsule), 0 until measured
        std::string   name;
        float         healthFrac;  // 0..1, the last fraction that could actually be read (1 until then)
    };
    std::unordered_map<X_Character*, Tracked> g_chars;
    std::uint32_t                             g_nextId = 1;

    // Game-thread state for the Minecraft link.
    std::uint32_t g_teleportSeq = 1;
    bool          g_teleportPending = true;
    bool          g_mcWasAlive = false;
    std::uint32_t g_lastMcPid = 0;
    bool          g_haveLastSet = false;
    McPoint       g_lastSet{};
    float         g_arrivingFor = 0.0f;
    bool          g_allowPlayerDamage = false;
    bool          g_takeover = false;
    DWORD         g_lastFrameTick = 0;
    McPoint       g_playerMc{};  // where MP2 has Max right now (Minecraft coords)
    bool          g_playerMcValid = false;
    // Minecraft's player walked off MP2's level: Max stays at his last place inside it (MP2 kills
    // him out there) and Minecraft is put back next to him, still looking where it looked.
    bool          g_outsideLevel = false;
    DWORD         g_lastBorderTeleport = 0;
    bool          g_keepLook = false;
    bool          g_playerByCamera = false;  // found through MP2's camera target
    DWORD         g_lastStatusLog = 0;

    Tracked& Track(X_Character* c)
    {
        auto [it, isNew] = g_chars.try_emplace(c, Tracked{ g_nextId, 0, 0.0f, {}, 1.0f });
        if (isNew) {
            ++g_nextId;
            it->second.name = SkinNameOf(c);
            mclog::Info("character #{} {} at {:p}", it->second.id, it->second.name, static_cast<void*>(c));
        }
        it->second.lastSeen = GetTickCount();
        return it->second;
    }

    X_Character* ById(std::uint32_t id)
    {
        for (auto& entry : g_chars)
            if (entry.second.id == id && GetTickCount() - entry.second.lastSeen < kStaleMs)
                return entry.first;
        return nullptr;
    }

    bool IsPlayerCandidate(X_Character* c)
    {
        bool result = false;
        Guarded([&] { result = api.getCharacterInput(c) != nullptr && !api.isAIActive(api.accessCharacterProperties(c)); });
        return result;
    }

    // MP2's skins name the characters we care about: Max ("MaxPayne..."), Mona ("Mona..."), and enemy
    // props that reuse those names ("...Enemy..."). Both the candidate being tested and the character
    // currently in charge go through this one predicate: read two ways, a "Mona...Enemy..." skin
    // matched one and not the other, and `named && !currentNamed` then handed the player to an
    // enemy and refused to hand it back.
    bool LooksLikePlayer(const std::string& name)
    {
        return (name.find("MaxPayne") != std::string::npos || name.find("Mona") != std::string::npos) && name.find("Enemy") == std::string::npos;
    }

    // The character's feet in Minecraft coordinates.
    bool FeetOf(X_Character* c, McPoint& out, float* yaw = nullptr)
    {
        const Matrix4x3* m = TransformOf(c);
        if (!m)
            return false;
        auto&           map = Mapping::Get();
        const float     off = Config::Get().feetOffset;
        const mp2::Vec3 up = map.DirToMp2(0, 1, 0);
        const mp2::Vec3 feet{ m->row[3].x - up.x * off, m->row[3].y - up.y * off, m->row[3].z - up.z * off };
        out = map.ToMc(feet);
        if (yaw)
            *yaw = map.YawOf(*m, Config::Get().forwardRow);
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    }

    // Health read, and whether there was one. The bindings are guesses on a guessed object, so a
    // character MP2 is halfway through destroying faults here; -1.0f used to stand for that, and a
    // -1.0f compared as health passes every "did it go down" test, which is how one fault used to
    // reach setDead. A failed read is no value at all, so a caller that needs one has to decide.
    bool TryHealth(X_Character* c, float& out)
    {
        float h = 0.0f;
        if (Guarded([&] { h = api.getHealth(c); }) && std::isfinite(h)) {
            out = h;
            return true;
        }
        return false;
    }

    // Health as 0..1 for the actor table. False when MP2 would not answer, `out` untouched then: the
    // caller keeps the fraction it read last frame, because the failure returning 0.0f wrote every
    // enemy to Minecraft as a corpse.
    bool HealthFrac(X_Character* c, float& out)
    {
        float max = 0.0f;
        Guarded([&] { max = api.getMaximumHealth(c); });
        if (!(max > 0.0f) || !std::isfinite(max))
            return false;
        float now = 0.0f;
        if (!TryHealth(c, now))
            return false;
        out = std::clamp(now / max, 0.0f, 1.0f);
        return true;
    }

    // A Minecraft hit on an MP2 character. MP2's own damage paths come first (hit reactions, death
    // animations, AI alerting); each is checked for whether the health actually went down, since
    // causeDamage without a shooting target was seen to do nothing at all.
    void Damage(X_Character* target, float amount)
    {
        static int logged = 0;
        float       before = 0.0f;
        const char* how = "causeDamage";
        const bool  haveBefore = TryHealth(target, before);
        // "Did it work": 1 health went down, 0 it did not, -1 health cannot be read at all. Every
        // tier below is picked by that answer, so -1 has to mean "stop here" and not "unchanged":
        // the last tier writes health and calls setDead, and a single unreadable read on the way must
        // never be enough to reach it.
        auto        took = [&] {
            float now = 0.0f;
            if (!haveBefore || !TryHealth(target, now))
                return -1;
            return now < before - 1e-4f ? 1 : 0;
        };
        Guarded([&] { g_origDamage(target, amount, 1.0f, 0, State().player, nullptr); });
        int         step = took();
        if (step == 0) {
            how = "explosionDamage";
            Guarded([&] {
                Vec3 at{};
                api.getHeadPosition(target, at);
                const void* source = State().player ? api.getOID(State().player) : nullptr;
                if (source)
                    api.explosionDamage(target, source, amount, at, 0, false, false);
            });
            step = took();
        }
        if (step == 0) {
            how = "health";
            const float left = (std::max)(0.0f, before - amount);
            Guarded([&] { api.setHealth(api.accessCharacterProperties(target), left); });
            if (left <= 0.0f) {
                how = "health + knocked over";
                Guarded([&] { api.knockOver(target); });
                Guarded([&] { g_origSetDead(target, true); });
            }
        } else if (step < 0) {
            // MP2's own damage path already ran and needs no health value; the fallbacks do, so they
            // are skipped rather than guessed. logged < 12 keeps this to a few lines per session.
            how = haveBefore ? "causeDamage (health became unreadable: fallbacks skipped)" : "causeDamage (health unreadable: fallbacks skipped)";
        }
        if (logged < 12) {
            ++logged;
            float after = 0.0f;
            if (TryHealth(target, after))
                mclog::Info("damage: {:.1f} to {:p} via {}: health {:.1f} -> {:.1f}", amount, static_cast<void*>(target), how, before, after);
            else
                mclog::Info("damage: {:.1f} to {:p} via {}: health {:.1f} -> unreadable", amount, static_cast<void*>(target), how, before);
        }
    }

    void HandleEvents()
    {
        auto&          st = State();
        auto&          link = Link::Get();
        const auto&    cfg = Config::Get();
        proto::McEvent ev{};
        while (link.PopEvent(ev)) {
            switch (ev.type) {
            case proto::kEvHitActor:
                if (X_Character* c = ById(ev.formId)) {
                    float max = 0.0f;
                    Guarded([&] { max = api.getMaximumHealth(c); });
                    const float amount = ev.a / 20.0f * max * cfg.enemyDamageScale;
                    Damage(c, amount);
                    if (cfg.diagnostics) {
                        float hp = 0.0f;
                        mclog::Info("hit #{} for {:.1f} MC -> {:.2f} MP2 (hp now {})", ev.formId, ev.a, amount,
                                    HealthFrac(c, hp) ? std::format("{:.0f}%", hp * 100.0f) : std::string("unreadable"));
                    }
                }
                break;
            case proto::kEvPlayerDied:
                if (st.player) {
                    mclog::Info("Minecraft player died: killing Max");
                    g_allowPlayerDamage = true;
                    float max = 100.0f;
                    Guarded([&] { max = api.getMaximumHealth(st.player); });
                    Guarded([&] { g_origDamage(st.player, max * 10.0f, 0.0f, 0, nullptr, nullptr); });
                    g_allowPlayerDamage = false;
                }
                break;
            case proto::kEvExplosion:
                for (auto& entry : g_chars) {
                    X_Character* c = entry.first;
                    if (c == st.player || GetTickCount() - entry.second.lastSeen > kStaleMs)
                        continue;
                    McPoint p;
                    if (!FeetOf(c, p))
                        continue;
                    const double d = std::sqrt((p.x - ev.a) * (p.x - ev.a) + (p.y + 0.9 - ev.b) * (p.y + 0.9 - ev.b) + (p.z - ev.c) * (p.z - ev.c));
                    const double reach = ev.d * 2.0;
                    if (d < reach) {
                        float max = 0.0f;
                        Guarded([&] { max = api.getMaximumHealth(c); });
                        Damage(c, static_cast<float>((1.0 - d / reach) * max * 1.5));
                    }
                }
                break;
            default:
                break;
            }
        }
    }

    void WriteActors()
    {
        auto&                             st = State();
        static std::vector<proto::ActorRecord> records;
        records.clear();
        const DWORD now = GetTickCount();
        for (auto& entry : g_chars) {
            X_Character* c = entry.first;
            Tracked&     t = entry.second;
            if (now - t.lastSeen > kStaleMs || c == st.player || records.size() >= proto::kMaxActors)
                continue;
            McPoint p;
            float   yaw = 0.0f;
            if (!FeetOf(c, p, &yaw))
                continue;
            if (t.height <= 0.0f) {
                Vec3  a{}, b{};
                float r = 0.0f;
                t.height = CapsuleOf(c, a, b, r) ? static_cast<float>((std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z)) + 2 * r) /
                                                                     Mapping::Get().UnitsPerBlock())
                                                 : 1.8f;
                if (!(t.height > 0.5f && t.height < 6.0f))
                    t.height = 1.8f;
            }
            bool dead = false, ai = false;
            Guarded([&] {
                dead = api.isCharacterDead(c);
                ai = api.isAIActive(api.accessCharacterProperties(c));
            });
            proto::ActorRecord r{};
            r.formId = t.id;
            r.flags = (dead ? proto::kActorDead : 0u) | (ai && !dead ? proto::kActorHostile | proto::kActorInCombat : 0u);
            r.x = static_cast<float>(p.x);
            r.y = static_cast<float>(p.y);
            r.z = static_cast<float>(p.z);
            r.yaw = yaw;
            r.width = 0.6f;
            r.height = t.height;
            // Out is the entry itself, so a read that fails leaves last frame's fraction standing:
            // publishing 0.0f instead would tell Minecraft every actor it can see is a corpse.
            HealthFrac(c, t.healthFrac);
            r.healthFrac = t.healthFrac;
            r.level = 10;
            strncpy_s(r.name, t.name.c_str(), _TRUNCATE);
            records.push_back(r);
        }
        Link::Get().WriteActors(records.data(), static_cast<std::uint32_t>(records.size()));
    }

    void BuildCamera(Runtime& st)
    {
        auto&        map = Mapping::Get();
        const double yaw = st.yaw * kDegToRad, pitch = st.pitch * kDegToRad;
        // Minecraft: yaw 0 faces +Z; pitch > 0 looks down.
        const McPoint fwd{ -std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch) };
        const McPoint right{ -std::cos(yaw), 0.0, -std::sin(yaw) };
        const McPoint up{ right.y * fwd.z - right.z * fwd.y, right.z * fwd.x - right.x * fwd.z, right.x * fwd.y - right.y * fwd.x };

        McPoint eye{ st.eyeX, st.eyeY, st.eyeZ };
        McPoint look = fwd;
        const float distance = st.mc.cameraDistance;
        if (st.mc.cameraMode == 1) {  // third person behind
            eye = { eye.x - fwd.x * distance, eye.y - fwd.y * distance, eye.z - fwd.z * distance };
        } else if (st.mc.cameraMode == 2) {  // third person in front, looking back
            eye = { eye.x + fwd.x * distance, eye.y + fwd.y * distance, eye.z + fwd.z * distance };
            look = { -fwd.x, -fwd.y, -fwd.z };
        }
        const McPoint camRight = st.mc.cameraMode == 2 ? McPoint{ -right.x, -right.y, -right.z } : right;

        const int fRow = std::clamp(Config::Get().forwardRow, 0, 2);
        const int uRow = fRow == 1 ? 2 : 1;
        const int rRow = 3 - fRow - uRow;
        st.camera.row[fRow] = map.DirToMp2(look.x, look.y, look.z);
        st.camera.row[uRow] = map.DirToMp2(up.x, up.y, up.z);
        st.camera.row[rRow] = map.DirToMp2(camRight.x, camRight.y, camRight.z);
        st.camera.row[3] = map.ToMp2(eye.x, eye.y, eye.z);
        st.fovDeg = st.mc.fovDeg > 1.0f ? st.mc.fovDeg : 70.0f;
    }

    void Interpolate(Runtime& st)
    {
        const auto& mc = st.mc;
        st.feetX = mc.x, st.feetY = mc.y, st.feetZ = mc.z;
        double eyeHeight = mc.eyeHeight;
        if (mc.tickQpc != 0 && mc.tickMs > 0.0f) {
            static const double qpcPerMs = [] {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                return double(f.QuadPart) / 1000.0;
            }();
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double t = std::clamp(double(now.QuadPart - mc.tickQpc) / qpcPerMs / mc.tickMs, 0.0, 1.0);
            st.feetX = mc.prevX + (mc.curX - mc.prevX) * t;
            st.feetY = mc.prevY + (mc.curY - mc.prevY) * t;
            st.feetZ = mc.prevZ + (mc.curZ - mc.prevZ) * t;
            eyeHeight = mc.tickEyeO + (mc.tickEye - mc.tickEyeO) * t;
        }
        st.eyeX = st.feetX;
        st.eyeY = st.feetY + eyeHeight;
        st.eyeZ = st.feetZ;
    }

    // Once per game frame, from the player's own update.
    void PerFrame(X_Character* player)
    {
        auto&       st = State();
        auto&       link = Link::Get();
        const DWORD nowTick = GetTickCount();
        const float delta = g_lastFrameTick ? (std::min)((nowTick - g_lastFrameTick) / 1000.0f, 0.25f) : 0.016f;
        g_lastFrameTick = nowTick;
        st.lastPlayerFrameMs = GetTickCount64();

        // Minecraft link state.
        const bool mcAlive = link.McAlive();
        static DWORD lastMcRead = 0;
        proto::McState fresh{};
        if (mcAlive && link.ReadMcState(fresh)) {
            st.mc = fresh;
            lastMcRead = nowTick;
        }
        st.haveMc = mcAlive && lastMcRead && nowTick - lastMcRead < 1000;
        const auto mcPid = link.McPid();
        if (mcAlive && (!g_mcWasAlive || (mcPid != 0 && mcPid != g_lastMcPid))) {
            mclog::Info("Minecraft connected (pid {})", mcPid);
            link.ResetOverlay();
            collision::Reset(++st.epoch);
            g_teleportPending = true;
        }
        if (mcAlive)
            g_lastMcPid = mcPid;
        g_mcWasAlive = mcAlive;
        st.mcInWorld = st.haveMc && (st.mc.flags & proto::kMcInWorld);
        const bool screenOpen = st.haveMc && (st.mc.flags & proto::kMcScreenOpen);
        if (screenOpen && !st.mcScreenOpen) {
            st.cursorX = st.viewportW / 2;
            st.cursorY = st.viewportH / 2;
        }
        st.mcScreenOpen = screenOpen;
        if (st.haveMc && st.mc.sensitivity > 0.0f)
            st.sensitivity = st.mc.sensitivity;

        // Scale and axes, from Max's capsule, the first time we see him.
        auto& map = Mapping::Get();
        if (!map.Calibrated()) {
            Vec3  a{}, b{};
            float r = 0.0f;
            if (CapsuleOf(player, a, b, r)) {
                map.Calibrate(a, b, r);
                collision::Reset(++st.epoch);
                g_teleportPending = true;
            }
        }

        // MP2 takes Max back for cut-scenes, scripted cameras and death.
        bool cinematic = false, dead = false;
        Guarded([&] {
            cinematic = api.isInCinematicMode(player);
            dead = api.isCharacterDead(player);
        });
        const bool takeover = cinematic || camera::PathActive() || dead;
        if (takeover != g_takeover) {
            if (takeover)
                mclog::Info("MP2 takes Max ({})", cinematic ? "cut-scene" : dead ? "dead" : "camera path");
            else
                mclog::Info("MP2 hands Max back");
            if (!takeover)
                g_teleportPending = true;  // Minecraft picks up wherever MP2 left him
            input::ReleaseAll();
        }
        g_takeover = takeover;

        // Off the edge of the level: back to where Max still is.
        if (g_outsideLevel) {
            g_outsideLevel = false;
            if (nowTick - g_lastBorderTeleport > 500) {
                g_lastBorderTeleport = nowTick;
                g_teleportPending = true;
                g_keepLook = true;
                mclog::Info("world border: Minecraft's player left MP2's level; putting them back by Max");
            }
        }

        // MP2 moved Max itself (level start, script, loaded save): resync Minecraft.
        float maxYaw = 0.0f;
        const bool haveFeet = FeetOf(player, g_playerMc, &maxYaw);
        g_playerMcValid = haveFeet && map.Calibrated();
        if (haveFeet && g_haveLastSet && st.puppeting) {
            const double dx = g_playerMc.x - g_lastSet.x, dy = g_playerMc.y - g_lastSet.y, dz = g_playerMc.z - g_lastSet.z;
            if (dx * dx + dy * dy + dz * dz > 4.0) {
                mclog::Info("MP2 moved Max ({:.1f} blocks); resyncing Minecraft", std::sqrt(dx * dx + dy * dy + dz * dz));
                g_teleportPending = true;
                g_haveLastSet = false;
            }
        }
        if ((g_teleportPending || !st.lookInitialized) && haveFeet && map.Calibrated()) {
            if (g_teleportPending)
                ++g_teleportSeq;
            g_teleportPending = false;
            if (!g_keepLook || !st.lookInitialized) {
                st.yaw = maxYaw;
                st.pitch = 0.0f;
            }
            g_keepLook = false;
            st.lookInitialized = true;
        }

        // Mouse look, Minecraft's formula.
        const int dx = st.lookDx.exchange(0), dy = st.lookDy.exchange(0);
        if (!st.mcScreenOpen && !takeover) {
            const float s = st.sensitivity * 0.6f + 0.2f;
            const float factor = s * s * s * 8.0f * 0.15f;
            st.yaw = std::fmod(st.yaw + dx * factor, 360.0f);
            st.pitch = std::clamp(st.pitch + dy * factor, -90.0f, 90.0f);
        }

        const bool ready = st.haveMc && st.mcInWorld && map.Calibrated() && !takeover;
        const bool arriving = ready && st.mc.teleportAck != g_teleportSeq;
        const bool puppet = ready && st.mc.teleportAck == g_teleportSeq;
        g_arrivingFor = arriving ? g_arrivingFor + delta : 0.0f;
        if (g_arrivingFor > 8.0f) {
            mclog::Info("Minecraft hasn't arrived after 8 s; teleporting it again");
            g_teleportPending = true;
            g_arrivingFor = 0.0f;
        }
        if (puppet != st.puppeting)
            mclog::Info("puppet {}", puppet ? "on (Minecraft drives Max)" : "off");
        st.puppeting = puppet;
        st.minecraftOwnsPlayer = puppet || arriving;
        st.gameOwnsInput = !st.minecraftOwnsPlayer;

        if (st.haveMc) {
            Interpolate(st);
            BuildCamera(st);
        }
        st.cameraValid = puppet;

        // Every few seconds: where both games think Max is (to check the mapping in the field).
        if (nowTick - g_lastStatusLog > 5000) {
            g_lastStatusLog = nowTick;
            const Matrix4x3* m = TransformOf(player);
            mclog::Info("status: puppet {} | MP2 origin ({:.2f}, {:.2f}, {:.2f}) -> MC ({:.2f}, {:.2f}, {:.2f}) yaw {:.0f} | MC feet ({:.2f}, {:.2f}, {:.2f}) ack {}/{} | look {:.0f}/{:.0f}",
                        puppet, m ? m->row[3].x : 0.0f, m ? m->row[3].y : 0.0f, m ? m->row[3].z : 0.0f, g_playerMc.x, g_playerMc.y, g_playerMc.z, maxYaw,
                        st.mc.x, st.mc.y, st.mc.z, st.mc.teleportAck, g_teleportSeq, st.yaw, st.pitch);
        }

        HandleEvents();
        WriteActors();
        if (st.haveMc && map.Calibrated())
            collision::Update(puppet ? McPoint{ st.mc.x, st.mc.y, st.mc.z } : g_playerMc);
    }

    // Minecraft's feet -> Max, after MP2's own physics (which we skipped) so nothing overrides it.
    void ApplyPuppet(X_Character* player)
    {
        if (!collision::InsideLevel({ State().feetX, State().feetY, State().feetZ })) {
            g_outsideLevel = true;
            return;
        }
        auto&            st = State();
        auto&            map = Mapping::Get();
        const auto&      cfg = Config::Get();
        Matrix4x3        m{};
        map.FacingYaw(st.yaw, cfg.forwardRow, m);
        const mp2::Vec3 up = map.DirToMp2(0, 1, 0);
        // First person: the camera sits inside Max's head, so his body steps back out of view.
        // MP2 still has him right next to the player (enemies see and shoot him there).
        double fx = st.feetX, fz = st.feetZ;
        if (st.mc.cameraMode == 0) {
            const double yaw = st.yaw * 0.017453292519943295;
            fx += std::sin(yaw) * cfg.bodyBehind;
            fz -= std::cos(yaw) * cfg.bodyBehind;
        }
        const mp2::Vec3 feet = map.ToMp2(fx, st.feetY, fz);
        m.row[3] = { feet.x + up.x * cfg.feetOffset, feet.y + up.y * cfg.feetOffset, feet.z + up.z * cfg.feetOffset };
        Guarded([&] { api.setTransform(api.accessCharacterProperties(player), m); });
        MovePhantom(RigidBodyOf(player), m);
        g_lastSet = { fx, st.feetY, fz };
        g_haveLastSet = true;
    }

    void __fastcall UpdatePrePhysics(X_Character* self, void*, const X_TimeUpdate& time)
    {
        auto& st = State();
        Track(self);
        const auto  known = st.player ? g_chars.find(st.player) : g_chars.end();
        const bool  stale = known == g_chars.end() || GetTickCount() - known->second.lastSeen > kStaleMs;
        const auto  target = reinterpret_cast<std::uintptr_t>(camera::Target());
        const auto  me = reinterpret_cast<std::uintptr_t>(self);
        // The camera target sits right next to its own character (+0xdc); anything within 64 KB
        // matched other characters' allocations in busy saves, which made the pick flicker.
        const bool  followed = target && target >= me && target - me < 0x1000;
        // While Minecraft drives the player the pick is frozen: switching characters here teleports
        // Max around (and reads the wrong character's feet), which resyncs Minecraft forever.
        if (self != st.player && !st.minecraftOwnsPlayer) {
            const auto&        name = g_chars[self].name;
            const bool         named = LooksLikePlayer(name);
            const bool         currentNamed = known != g_chars.end() && LooksLikePlayer(known->second.name);
            const bool         maySteal = stale || (named && !currentNamed);
            if (followed && !camera::PathActive()) {
                if (maySteal) {
                    st.player = self;
                    g_playerByCamera = true;
                    mclog::Info("player is #{} ({}): MP2's camera follows it (+{:#x})", g_chars[self].id, name, target - me);
                }
            } else if (!g_playerByCamera && maySteal && IsPlayerCandidate(self)) {
                st.player = self;
                mclog::Info("player is #{} ({}): input-driven, AI off", g_chars[self].id, name);
            }
        }
        if (self == st.player)
            PerFrame(self);
        g_origPre(self, time);
    }

    void __fastcall UpdateCharacterPhysics(X_Character* self, void*, float dt, P_Camera* camera)
    {
        // MP2's physics still runs for Max: it is where MP2 tracks which room he is in, and MP2
        // only draws the rooms visible from there (skipping it left new rooms black). It gets
        // no input while Minecraft has him, and updatePostPhysics puts him where Minecraft says.
        g_origPhysics(self, dt, camera);
    }

    void __fastcall UpdatePostPhysics(X_Character* self, void*, const X_TimeUpdate& time)
    {
        g_origPost(self, time);
        if (self == State().player && State().puppeting)
            ApplyPuppet(self);
    }

    void __fastcall CauseDamage(X_Character* self, void*, float amount, float force, int deathAnim, X_Character* attacker, const void* target)
    {
        auto& st = State();
        if (self == st.player && st.minecraftOwnsPlayer && !g_allowPlayerDamage) {
            // Minecraft's health is authoritative: send it there as a fraction of Max's health
            // (protocol units: "Skyrim damage" x 100, which SkyCraft's mod divides by 5 -> 100 = 20 MC).
            float max = 100.0f;
            Guarded([&] { max = api.getMaximumHealth(self); });
            const float frac = max > 0.0f ? amount / max : 0.0f;
            const auto  units = static_cast<std::int32_t>(frac * 100.0f * 100.0f * Config::Get().playerDamageScale);
            std::uint32_t attackerId = 0;
            if (attacker && g_chars.contains(attacker))
                attackerId = g_chars[attacker].id;
            Link::Get().PushInput(proto::kInHurt, proto::kHurtProjectile, units, static_cast<std::int32_t>(attackerId), 0);
            return;
        }
        g_origDamage(self, amount, force, deathAnim, attacker, target);
    }
}

namespace
{
    // While Minecraft has Max, only Minecraft decides when he dies (its health is the real one):
    // MP2's own ways of killing him outright (level-edge kill zones, falls, scripts) are refused.
    bool ProtectedProps(X_CharacterProperties* props)
    {
        auto& st = State();
        if (!st.player || !st.minecraftOwnsPlayer || g_allowPlayerDamage)
            return false;
        X_CharacterProperties* mine = nullptr;
        Guarded([&] { mine = api.accessCharacterProperties(st.player); });
        return props == mine;
    }

    void LogRefused(const char* what)
    {
        static DWORD last = 0;
        if (GetTickCount() - last > 2000) {
            last = GetTickCount();
            mclog::Info("refused MP2 {} for Max (Minecraft decides his health)", what);
        }
    }

    void __fastcall SetHealth(X_CharacterProperties* self, void*, float health)
    {
        if (ProtectedProps(self)) {
            float now = health;
            Guarded([&] { now = api.getHealth(State().player); });
            if (health < now) {
                LogRefused("damage");
                return;
            }
        }
        g_origSetHealth(self, health);
    }

    void __fastcall SetCharacterDead(X_Character* self, void*, bool dead)
    {
        auto& st = State();
        if (dead && self == st.player && st.minecraftOwnsPlayer && !g_allowPlayerDamage) {
            LogRefused("death");
            return;
        }
        g_origSetDead(self, dead);
    }

    void __fastcall SetDying(X_CharacterProperties* self, void*, bool dying)
    {
        if (dying && ProtectedProps(self)) {
            LogRefused("dying");
            return;
        }
        g_origSetDying(self, dying);
    }
}

bool characters::Install()
{
    bool ok = true;
    ok &= InstallHook(api.updatePrePhysics, reinterpret_cast<void*>(&UpdatePrePhysics), g_origPre, "X_Character::updatePrePhysics");
    ok &= InstallHook(api.updateCharacterPhysics, reinterpret_cast<void*>(&UpdateCharacterPhysics), g_origPhysics, "X_Character::updateCharacterPhysics");
    ok &= InstallHook(api.updatePostPhysics, reinterpret_cast<void*>(&UpdatePostPhysics), g_origPost, "X_Character::updatePostPhysics");
    ok &= InstallHook(api.causeDamageTarget, reinterpret_cast<void*>(&CauseDamage), g_origDamage, "X_Character::causeDamage");
    ok &= InstallHook(reinterpret_cast<void*>(api.setHealth), reinterpret_cast<void*>(&SetHealth), g_origSetHealth, "X_CharacterProperties::setHealth");
    ok &= InstallHook(api.setCharacterDead, reinterpret_cast<void*>(&SetCharacterDead), g_origSetDead, "X_Character::setCharacterDead");
    ok &= InstallHook(api.setDying, reinterpret_cast<void*>(&SetDying), g_origSetDying, "X_CharacterProperties::setDying");
    return ok;
}

void characters::OnLevelChange()
{
    auto& st = State();
    g_chars.clear();
    st.player = nullptr;
    g_playerByCamera = false;
    st.puppeting = false;
    st.minecraftOwnsPlayer = false;
    st.gameOwnsInput = true;
    st.cameraValid = false;
    g_haveLastSet = false;
    g_teleportPending = true;
    // Belongs to the level that just went away, and nothing here may act on it: "Minecraft is still
    // arriving" (so a fresh teleport's 8 s timeout is inherited and never fires), "Minecraft's player
    // is outside the level" (so the first frame of the new level yanks it back to a position in the
    // old one) and "keep the look direction" (which would apply the old level's yaw to the new one).
    g_arrivingFor = 0.0f;
    g_outsideLevel = false;
    g_lastBorderTeleport = 0;
    g_keepLook = false;
    // g_teleportSeq stays: Minecraft matches SkyState::teleportSeq against the last one it applied,
    // so it has to keep climbing across levels. g_nextId stays for the same reason (ids are handed to
    // Minecraft and remembered there). g_lastMcPid and g_mcWasAlive stay: they track the Minecraft
    // process, not the level, and zeroing them would make every level change look like a reconnect
    // (a second epoch bump and a "Minecraft connected" line that never happened).
    ++st.levelId;
    collision::Reset(++st.epoch);
    Link::Get().WriteActors(nullptr, 0);
}

void characters::OnPresent(std::uint32_t width, std::uint32_t height)
{
    auto& st = State();
    auto& link = Link::Get();
    link.Heartbeat();
    st.viewportW = static_cast<int>(width);
    st.viewportH = static_cast<int>(height);

    // No player update lately: MP2 is paused, in a menu or loading. It gets all input.
    const bool paused = !st.player || GetTickCount64() - st.lastPlayerFrameMs > kPausedMs;
    if (paused && !st.gameOwnsInput) {
        input::ReleaseAll();
        st.gameOwnsInput = true;
    }
    if (paused)
        st.cameraValid = false;

    proto::SkyState sky{};
    const bool inGame = st.player && g_playerMcValid;
    sky.flags = (inGame ? proto::kSkyInGame : 0u) | (paused ? proto::kSkyMenuOpen : 0u) | (!inGame ? proto::kSkyLoading : 0u);
    sky.worldId = st.levelId;
    sky.collisionEpoch = st.epoch;
    sky.posX = g_playerMc.x;
    sky.posY = g_playerMc.y;
    sky.posZ = g_playerMc.z;
    sky.yaw = st.yaw;
    sky.pitch = st.pitch;
    sky.teleportSeq = g_teleportSeq;
    sky.viewportW = width;
    sky.viewportH = height;
    sky.gameHour = Config::Get().gameHour;
    link.WriteSkyState(sky);
}

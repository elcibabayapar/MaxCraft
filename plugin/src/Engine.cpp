#include "Engine.h"

#include "Hook.h"
#include "Log.h"

#include <Windows.h>

mp2::Api mp2::api{};

namespace
{
    template <class T>
    bool Bind(T& out, const wchar_t* module, const char* symbol)
    {
        HMODULE dll = GetModuleHandleW(module);
        FARPROC proc = dll ? GetProcAddress(dll, symbol) : nullptr;
        if (!proc) {
            mclog::Info("missing symbol {}", symbol);
            return false;
        }
        out = reinterpret_cast<T>(proc);
        return true;
    }
}

bool mp2::Resolve()
{
    constexpr auto objects = L"X_GameObjectsMFC.dll";
    constexpr auto physics = L"X_PhysicalSimulationMFC.dll";

    bool ok = true;
    ok &= Bind(api.getHealth, objects, "?getHealth@X_Character@@QBEMXZ");
    ok &= Bind(api.getMaximumHealth, objects, "?getMaximumHealth@X_Character@@QBEMXZ");
    ok &= Bind(api.isCharacterDead, objects, "?isCharacterDead@X_Character@@QBE_NXZ");
    ok &= Bind(api.isInCinematicMode, objects, "?isInCinematicMode@X_Character@@QBE_NXZ");
    ok &= Bind(api.getCharacterInput, objects, "?getCharacterInput@X_Character@@QBEPAVX_CharacterInput@@XZ");
    ok &= Bind(api.accessCharacterProperties, objects, "?accessCharacterProperties@X_Character@@QAEAAVX_CharacterProperties@@XZ");
    ok &= Bind(api.getPhysicalCharacter, objects, "?getPhysicalCharacter@X_Character@@QBEPBVX_PhysicalCharacter@@XZ");
    ok &= Bind(api.getSkinName, objects, "?getSkinName@X_Character@@QBEABV?$basic_string@DU?$char_traits@D@std@@V?$R_Allocator@D@@@std@@XZ");
    ok &= Bind(api.causeDamage, objects, "?causeDamage@X_Character@@QAEXMMW4DeathAnim@1@PAV1@PBVX_SharedDBShootingTarget@@@Z");
    api.causeDamageTarget = reinterpret_cast<void*>(api.causeDamage);
    ok &= Bind(api.getOID, objects, "?getOID@X_Character@@UBEPBVX_ObjectID@@XZ");
    ok &= Bind(api.explosionDamage, objects, "?explosionDamage@X_Character@@QAEXABVX_ObjectID@@MABV?$M_Vector3Template@M@@H_N2@Z");
    ok &= Bind(api.knockOver, objects, "?knockOver@X_Character@@QAEXXZ");
    ok &= Bind(api.getHeadPosition, objects, "?getHeadPosition@X_Character@@QBEXAAV?$M_Vector3Template@M@@@Z");

    ok &= Bind(api.getTransform, objects, "?getTransform@X_CharacterProperties@@QBEABV?$M_Matrix4x3Template@M@@XZ");
    ok &= Bind(api.setTransform, objects, "?setTransform@X_CharacterProperties@@QAEXABV?$M_Matrix4x3Template@M@@@Z");
    ok &= Bind(api.isAIActive, objects, "?isAIActive@X_CharacterProperties@@QBE_NXZ");
    ok &= Bind(api.setHealth, objects, "?setHealth@X_CharacterProperties@@QAEXM@Z");

    ok &= Bind(api.getRigidBodyCharacter, physics, "?getRigidBodyCharacter@X_PhysicalCharacter@@QBEPAVX_RigidBodyCharacter@@XZ");
    ok &= Bind(api.getCapsuleExtent, physics, "?getCapsuleExtent@X_RigidBodyCharacter@@QBEXAAV?$M_Vector3Template@M@@0AAM@Z");

    ok &= Bind(api.isCameraPathActive, objects, "?isCameraPathActive@X_CameraImplementation@@UBE_NXZ");
    ok &= Bind(api.setFOV, objects, "?setFOV@X_LevelRuntimeCamera@@QAEXM@Z");
    ok &= Bind(api.getLevelCamera, objects, "?getCamera@X_LevelRuntimeCamera@@QAEPAVP_Camera@@XZ");
    ok &= Bind(api.getViewconeMatrix, L"e2mfc.dll", "?getViewconeMatrix@P_Camera@@QBEABV?$M_Matrix4x3Template@M@@XZ");
    ok &= Bind(api.getObjectPosition, L"e2mfc.dll", "?getPosition@P_BaseObject@@UBEABV?$M_Vector3Template@M@@XZ");
    ok &= Bind(api.setObjectPosition, L"e2mfc.dll", "?setPosition@P_BaseObject@@UAEXABV?$M_Vector3Template@M@@@Z");
    ok &= Bind(api.calculateObjectToWorld, L"e2mfc.dll", "?calculateObjectToWorldMatrix@P_BaseObject@@IAEXXZ");
    ok &= Bind(api.getFOV, objects, "?getFOV@X_LevelRuntimeCamera@@QBEMXZ");

    ok &= Bind(api.getVertexCount, physics, "?getVertexCount@X_HavokGeometry@@QBEHXZ");
    ok &= Bind(api.getVertex, physics, "?getVertex@X_HavokGeometry@@QBEABV?$M_Vector3Template@M@@H@Z");
    ok &= Bind(api.getTriangleCount, physics, "?getTriangleCount@X_HavokGeometry@@QBEHXZ");
    ok &= Bind(api.getVertexIndex, physics, "?getVertexIndex@X_HavokGeometry@@QBEHHH@Z");

    ok &= Bind(api.updatePrePhysics, objects, "?updatePrePhysics@X_Character@@QAEXABVX_TimeUpdate@@@Z");
    ok &= Bind(api.updateCharacterPhysics, objects, "?updateCharacterPhysics@X_Character@@QAEXMPAVP_Camera@@@Z");
    ok &= Bind(api.updatePostPhysics, objects, "?updatePostPhysics@X_Character@@QAEXABVX_TimeUpdate@@@Z");
    ok &= Bind(api.cameraUpdate, objects, "?update@X_CameraImplementation@@UAEXPBVX_CameraTarget@@ABVX_TimeUpdate@@@Z");
    ok &= Bind(api.cameraGetCurrentMatrix, objects, "?getCurrentMatrix@X_CameraImplementation@@UBEABV?$M_Matrix4x3Template@M@@XZ");
    ok &= Bind(api.cameraInitLevel, objects, "?initLevel@X_CameraImplementation@@UAEXPBVX_LevelRuntimeRoomContainer@@@Z");
    ok &= Bind(api.cameraDeinitLevel, objects, "?deinitLevel@X_CameraImplementation@@UAEXXZ");
    ok &= Bind(api.levelCameraUpdateVisibility, objects, "?updateVisibility@X_LevelRuntimeCamera@@QAEXXZ");
    ok &= Bind(api.setCharacterDead, objects, "?setCharacterDead@X_Character@@QAEX_N@Z");
    ok &= Bind(api.setDying, objects, "?setDying@X_CharacterProperties@@QAEX_N@Z");
    ok &= Bind(api.allocateRigidBodyRoom, physics, "?allocateRigidBodyRoom@X_RigidBodyRoom@@SIPAV1@PAVX_HavokGeometry@@ABV?$M_Matrix4x3Template@M@@@Z");

    mclog::Info("engine symbols {}", ok ? "resolved" : "INCOMPLETE");
    return ok;
}

const mp2::Matrix4x3* mp2::TransformOf(X_Character* c)
{
    const Matrix4x3* m = nullptr;
    Guarded([&] { m = &api.getTransform(api.accessCharacterProperties(c)); });
    return m;
}

mp2::X_RigidBodyCharacter* mp2::RigidBodyOf(X_Character* c)
{
    X_RigidBodyCharacter* body = nullptr;
    Guarded([&] {
        if (const auto* physical = api.getPhysicalCharacter(c))
            body = api.getRigidBodyCharacter(physical);
    });
    return body;
}

bool mp2::CapsuleOf(X_Character* c, Vec3& a, Vec3& b, float& radius)
{
    X_RigidBodyCharacter* body = RigidBodyOf(c);
    return body && Guarded([&] { api.getCapsuleExtent(body, a, b, radius); });
}

std::string mp2::SkinNameOf(X_Character* c)
{
    // VC7.1 std::basic_string<char, ..., R_Allocator>: {allocator (padded to 4), union{char[16], char*}, size, capacity}.
    char buffer[24]{};
    Guarded([&] {
        const auto*       s = static_cast<const std::uint8_t*>(api.getSkinName(c));
        const std::size_t size = *reinterpret_cast<const std::uint32_t*>(s + 20);
        const std::size_t capacity = *reinterpret_cast<const std::uint32_t*>(s + 24);
        if (size > 256 || capacity < size)
            return;
        const char* text = capacity >= 16 ? *reinterpret_cast<const char* const*>(s + 4) : reinterpret_cast<const char*>(s + 4);
        for (std::size_t i = 0; i < size && i + 1 < sizeof(buffer); ++i)
            buffer[i] = (text[i] >= 32 && text[i] < 127) ? text[i] : '?';
    });
    return buffer;
}

void mp2::MovePhantom(X_RigidBodyCharacter* body, const Matrix4x3& m)
{
    if (!body)
        return;
    Guarded([&] {
        using Fn = void(__thiscall*)(X_RigidBodyCharacter*, const Matrix4x3&);
        auto fn = reinterpret_cast<Fn>((*reinterpret_cast<void***>(body))[kPhantomSetDisplayToWorld]);
        fn(body, m);
    });
}

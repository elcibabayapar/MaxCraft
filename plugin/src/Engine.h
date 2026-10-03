#pragma once

// Max Payne 2's engine, bound by name from its DLL exports (docs/DESIGN.md §3). Every member
// function is x86 __thiscall; the static ones marked "SI" in their mangled names are __fastcall.

#include <cstdint>
#include <string>

namespace mp2
{
    struct X_Character;
    struct X_CharacterProperties;
    struct X_CharacterInput;
    struct X_PhysicalCharacter;
    struct X_RigidBodyCharacter;
    struct X_CameraImplementation;
    struct X_CameraTarget;
    struct X_LevelRuntimeCamera;
    struct X_LevelRuntimeRoomContainer;
    struct X_HavokGeometry;
    struct X_RigidBodyRoom;
    struct X_TimeUpdate;
    struct P_Camera;

    struct Vec3
    {
        float x, y, z;
    };

    // M_Matrix4x3Template<float>, row-vector convention: p' = p.x*row[0] + p.y*row[1] + p.z*row[2] + row[3].
    struct Matrix4x3
    {
        Vec3 row[4];
    };
    static_assert(sizeof(Matrix4x3) == 48);

    // X_RigidBodyCharacter is a phantom: vtable slot 13 is
    // X_RigidBodyPhantom::setDisplayToWorldTransform(const M_Matrix4x3Template<float>&)
    // (read from ??_7X_RigidBodyCharacter@@6B@ in X_PhysicalSimulationMFC.dll).
    constexpr int kPhantomSetDisplayToWorld = 13;

    struct Api
    {
        // X_Character
        float(__thiscall* getHealth)(const X_Character*);
        float(__thiscall* getMaximumHealth)(const X_Character*);
        bool(__thiscall* isCharacterDead)(const X_Character*);
        bool(__thiscall* isInCinematicMode)(const X_Character*);
        X_CharacterInput*(__thiscall* getCharacterInput)(const X_Character*);
        X_CharacterProperties*(__thiscall* accessCharacterProperties)(X_Character*);
        const X_PhysicalCharacter*(__thiscall* getPhysicalCharacter)(const X_Character*);
        const void*(__thiscall* getSkinName)(const X_Character*);  // const std::string& (VC7.1 layout)
        void(__thiscall* causeDamage)(X_Character*, float, float, int, X_Character*, const void*);

        // X_CharacterProperties
        const Matrix4x3&(__thiscall* getTransform)(const X_CharacterProperties*);
        void(__thiscall* setTransform)(X_CharacterProperties*, const Matrix4x3&);
        bool(__thiscall* isAIActive)(const X_CharacterProperties*);
        void(__thiscall* setHealth)(X_CharacterProperties*, float);

        // X_PhysicalCharacter / X_RigidBodyCharacter
        X_RigidBodyCharacter*(__thiscall* getRigidBodyCharacter)(const X_PhysicalCharacter*);
        void(__thiscall* getCapsuleExtent)(const X_RigidBodyCharacter*, Vec3&, Vec3&, float&);

        // X_CameraImplementation
        bool(__thiscall* isCameraPathActive)(const X_CameraImplementation*);

        // X_LevelRuntimeCamera / P_Camera
        P_Camera*(__thiscall* getLevelCamera)(X_LevelRuntimeCamera*);
        const Matrix4x3&(__thiscall* getViewconeMatrix)(const P_Camera*);
        const Vec3&(__thiscall* getObjectPosition)(const P_Camera*);       // P_BaseObject::getPosition
        void(__thiscall* setObjectPosition)(P_Camera*, const Vec3&);       // P_BaseObject::setPosition
        void(__thiscall* calculateObjectToWorld)(P_Camera*);              // P_BaseObject::calculateObjectToWorldMatrix
        void(__thiscall* setFOV)(X_LevelRuntimeCamera*, float);
        float(__thiscall* getFOV)(const X_LevelRuntimeCamera*);

        // X_HavokGeometry
        int(__thiscall* getVertexCount)(const X_HavokGeometry*);
        const Vec3&(__thiscall* getVertex)(const X_HavokGeometry*, int);
        int(__thiscall* getTriangleCount)(const X_HavokGeometry*);
        int(__thiscall* getVertexIndex)(const X_HavokGeometry*, int, int);

        // Hook targets
        void* updatePrePhysics;        // void X_Character::updatePrePhysics(const X_TimeUpdate&)
        void* updateCharacterPhysics;  // void X_Character::updateCharacterPhysics(float, P_Camera*)
        void* updatePostPhysics;       // void X_Character::updatePostPhysics(const X_TimeUpdate&)
        void* causeDamageTarget;       // X_Character::causeDamage
        void* cameraUpdate;            // void X_CameraImplementation::update(const X_CameraTarget*, const X_TimeUpdate&)
        void* cameraGetCurrentMatrix;  // const Matrix4x3& X_CameraImplementation::getCurrentMatrix() const
        void* cameraInitLevel;         // void X_CameraImplementation::initLevel(const X_LevelRuntimeRoomContainer*)
        void* cameraDeinitLevel;       // void X_CameraImplementation::deinitLevel()
        void* levelCameraUpdateVisibility;  // void X_LevelRuntimeCamera::updateVisibility()
        void* allocateRigidBodyRoom;   // static X_RigidBodyRoom* __fastcall(X_HavokGeometry*, const Matrix4x3&)
    };

    extern Api api;

    bool Resolve();

    // Engine helpers built on the bindings (all guarded against bad guesses).
    const Matrix4x3*      TransformOf(X_Character* c);
    X_RigidBodyCharacter* RigidBodyOf(X_Character* c);
    bool                  CapsuleOf(X_Character* c, Vec3& a, Vec3& b, float& radius);
    std::string           SkinNameOf(X_Character* c);
    void                  MovePhantom(X_RigidBodyCharacter* body, const Matrix4x3& m);
}

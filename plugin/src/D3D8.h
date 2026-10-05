#pragma once

// The slice of Direct3D 8 MaxCraft uses. The Windows SDK no longer ships d3d8.h, so the types,
// constants and vtable slots are declared here (values from the DirectX 8 SDK headers).
// COM methods are called by vtable index: VCall<Index, Ret>(object, args...).
//
// Three of the slots below are reverse-engineered and are flagged where they are declared:
// surf::GetDesc (8), tex::LockRect/UnlockRect (16/17), and SurfaceDesc's field order. None of them
// matches the published IDirect3DSurface8 / IDirect3DTexture8 vtables, they cannot all be brought
// into agreement by one base offset, and the two that render correctly in every captured session
// are therefore left as they are. Render.cpp checks them at runtime once per device and logs which
// candidate answered - re-verify all three on any d3d8.dll or driver change before editing.

#include <Windows.h>

#include <cstdint>

namespace d3d8
{
    template <int Index, class Ret = HRESULT, class... Args>
    Ret VCall(void* object, Args... args)
    {
        using Fn = Ret(__stdcall*)(void*, Args...);
        return reinterpret_cast<Fn*>(*static_cast<void***>(object))[Index](object, args...);
    }

    inline void* VSlot(void* object, int index) { return (*static_cast<void***>(object))[index]; }
    inline void Release(void* object)
    {
        if (object)
            VCall<2, ULONG>(object);
    }

    constexpr UINT kSdkVersion = 220;

    enum Format : UINT
    {
        kFmtUnknown = 0,
        kFmtA8R8G8B8 = 21,
        kFmtX8R8G8B8 = 22,
    };

    struct PresentParameters
    {
        UINT  BackBufferWidth;
        UINT  BackBufferHeight;
        UINT  BackBufferFormat;
        UINT  BackBufferCount;
        UINT  MultiSampleType;
        UINT  SwapEffect;
        HWND  hDeviceWindow;
        BOOL  Windowed;
        BOOL  EnableAutoDepthStencil;
        UINT  AutoDepthStencilFormat;
        DWORD Flags;
        UINT  FullScreen_RefreshRateInHz;
        UINT  FullScreen_PresentationInterval;
    };

    struct DisplayMode
    {
        UINT Width, Height, RefreshRate, Format;
    };

    // REVERSE-ENGINEERED, and neither the SDK's layout nor a subset aligned to it: the SDK declares
    // Format, Type, Usage, MultiSampleType, MultiSampleQuality, SwapEffect, Pool, Size, Width,
    // Height, so with this field order Width and Height are read from where the SDK keeps SwapEffect
    // and Pool. It is kept because it is what every captured session read with, and the evidence it
    // produced looks right (the depth line reads 1920x1080, format 75 = D3DFMT_D24S8, no MSAA), but
    // a "1920x1080" here is two words of a structure nobody has checked against the real one.
    // Render.cpp's once-per-device surface check reads the words raw, both through this file's slot
    // and through the SDK's, so the layout can be settled in one session; do not read width, height,
    // format or multisample out of this struct without that log line in front of you.
    struct SurfaceDesc
    {
        UINT  Format;
        UINT  Type;
        DWORD Usage;
        UINT  Pool;
        UINT  Size;
        UINT  MultiSampleType;
        UINT  Width;
        UINT  Height;
    };

    struct Viewport
    {
        DWORD X, Y, Width, Height;
        float MinZ, MaxZ;
    };

    struct LockedRect
    {
        INT   Pitch;
        void* pBits;
    };

    struct DeviceCreationParameters
    {
        UINT  AdapterOrdinal;
        UINT  DeviceType;
        HWND  hFocusWindow;
        DWORD BehaviorFlags;
    };

    struct Matrix
    {
        float m[4][4];
    };

    // IDirect3D8
    namespace d3d
    {
        constexpr int GetAdapterDisplayMode = 8;
        constexpr int CreateDevice = 15;
    }

    // IDirect3DDevice8
    namespace dev
    {
        constexpr int Reset = 14;
        constexpr int Present = 15;
        constexpr int GetBackBuffer = 16;
        constexpr int CreateTexture = 20;
        constexpr int CreateVertexBuffer = 23;
        constexpr int GetCreationParameters = 9;
        constexpr int GetRenderTarget = 32;
        constexpr int GetDepthStencilSurface = 33;
        constexpr int BeginScene = 34;
        constexpr int EndScene = 35;
        constexpr int Clear = 36;
        constexpr int SetTransform = 37;
        constexpr int GetTransform = 38;
        constexpr int SetViewport = 40;
        constexpr int GetViewport = 41;
        constexpr int SetRenderState = 50;
        constexpr int ApplyStateBlock = 54;
        constexpr int CaptureStateBlock = 55;
        constexpr int DeleteStateBlock = 56;
        constexpr int CreateStateBlock = 57;
        constexpr int SetTexture = 61;
        constexpr int SetTextureStageState = 63;
        constexpr int DrawPrimitive = 70;
        constexpr int DrawPrimitiveUP = 72;
        constexpr int SetVertexShader = 76;
        constexpr int SetVertexShaderConstant = 79;
        constexpr int SetStreamSource = 83;
        constexpr int SetPixelShader = 88;
    }

    // IDirect3DSurface8
    //
    // REVERSE-ENGINEERED, and reported inconsistent with the published IDirect3DSurface8 vtable: the
    // SDK's GetLevelDesc is not slot 8, and no single base offset makes both 8 and 10 the same call
    // under two different headers. Slot 8 is what has always been used and it answered with a sane
    // description in every captured session (the depth log reads 1920x1080 / D3DFMT_D24S8 / no MSAA),
    // so it stays. GetDescAlt is the slot reported to be the SDK's; Render.cpp reads it once per
    // device and logs which one described the surface. Change one only on the strength of that log.
    namespace surf
    {
        constexpr int GetDesc = 8;
        constexpr int GetDescAlt = 10;
    }

    // IDirect3DTexture8
    //
    // REVERSE-ENGINEERED, and reported inconsistent with surf's: if GetDesc is really slot 8 then the
    // lock pair that goes with it is not 16/17, and the two accounts put the pair more than two slots
    // apart. Both sets work in the captured sessions (no "texture: lock ... failed" lines, and the
    // atlas probe reads a plausible texel), so both stay and Render.cpp tries both once per device.
    namespace tex
    {
        constexpr int LockRect = 16;
        constexpr int UnlockRect = 17;
        constexpr int LockRectAlt = 13;
        constexpr int UnlockRectAlt = 14;
    }

    // IDirect3DVertexBuffer8
    namespace vb
    {
        constexpr int Lock = 11;
        constexpr int Unlock = 12;
    }

    enum : UINT
    {
        kDevTypeHal = 1,
        kSwapDiscard = 1,
        kCreateSoftwareVp = 0x20,
        kPoolDefault = 0,
        kPoolManaged = 1,
        kBackBufferMono = 0,
        kStateBlockAll = 1,

        kTsView = 2,
        kTsProjection = 3,
        kTsWorld = 256,

        kPtLineList = 2,
        kPtTriangleList = 4,
        kPtTriangleStrip = 5,

        kFvfXyz = 0x002,
        kFvfXyzRhw = 0x004,
        kFvfDiffuse = 0x040,
        kFvfTex1 = 0x100,

        kUsageWriteOnly = 0x8,
    };

    enum RenderState : UINT
    {
        kRsZEnable = 7,
        kRsFillMode = 8,
        kRsShadeMode = 9,
        kRsZWriteEnable = 14,
        kRsAlphaTestEnable = 15,
        kRsSrcBlend = 19,
        kRsDestBlend = 20,
        kRsCullMode = 22,
        kRsZFunc = 23,
        kRsAlphaRef = 24,
        kRsAlphaFunc = 25,
        kRsAlphaBlendEnable = 27,
        kRsFogEnable = 28,
        kRsSpecularEnable = 29,
        kRsStencilEnable = 52,
        kRsClipping = 136,
        kRsLighting = 137,
        kRsColorVertex = 141,
        kRsColorWriteEnable = 168,
        kRsBlendOp = 171,
    };

    enum TextureStageState : UINT
    {
        kTssColorOp = 1,
        kTssColorArg1 = 2,
        kTssColorArg2 = 3,
        kTssAlphaOp = 4,
        kTssAlphaArg1 = 5,
        kTssAlphaArg2 = 6,
        kTssTexCoordIndex = 11,
        kTssAddressU = 13,
        kTssAddressV = 14,
        kTssMagFilter = 16,
        kTssMinFilter = 17,
        kTssMipFilter = 18,
        kTssTextureTransformFlags = 24,
    };

    enum : UINT
    {
        kBlendZero = 1,
        kBlendOne = 2,
        kBlendSrcAlpha = 5,
        kBlendInvSrcAlpha = 6,
        kBlendOpAdd = 1,
        kCmpLessEqual = 4,
        kCmpGreaterEqual = 7,
        kCullNone = 1,
        kFillSolid = 3,
        kShadeGouraud = 2,
        kTopDisable = 1,
        kTopSelectArg1 = 2,
        kTopModulate = 4,
        kTaDiffuse = 0,
        kTaTexture = 2,
        kTexfNone = 0,
        kTexfPoint = 1,
        kTaddressWrap = 1,
        kTaddressClamp = 3,
    };
}

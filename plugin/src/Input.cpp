#include "Input.h"

#include "Config.h"
#include "Hook.h"
#include "Log.h"
#include "Runtime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    // DirectInput scan code -> SDL scancode (USB HID usage), as SkyCraft maps Skyrim's.
    constexpr auto kDikToSdl = [] {
        std::array<std::uint16_t, 256> t{};
        t[0x01] = 41;
        for (int i = 0; i < 9; ++i)
            t[0x02 + i] = static_cast<std::uint16_t>(30 + i);
        t[0x0B] = 39;
        t[0x0C] = 45, t[0x0D] = 46, t[0x0E] = 42, t[0x0F] = 43;
        t[0x10] = 20, t[0x11] = 26, t[0x12] = 8, t[0x13] = 21, t[0x14] = 23;
        t[0x15] = 28, t[0x16] = 24, t[0x17] = 12, t[0x18] = 18, t[0x19] = 19;
        t[0x1A] = 47, t[0x1B] = 48, t[0x1C] = 40, t[0x1D] = 224;
        t[0x1E] = 4, t[0x1F] = 22, t[0x20] = 7, t[0x21] = 9, t[0x22] = 10;
        t[0x23] = 11, t[0x24] = 13, t[0x25] = 14, t[0x26] = 15;
        t[0x27] = 51, t[0x28] = 52, t[0x29] = 53, t[0x2A] = 225, t[0x2B] = 49;
        t[0x2C] = 29, t[0x2D] = 27, t[0x2E] = 6, t[0x2F] = 25, t[0x30] = 5;
        t[0x31] = 17, t[0x32] = 16, t[0x33] = 54, t[0x34] = 55, t[0x35] = 56;
        t[0x36] = 229, t[0x37] = 85, t[0x38] = 226, t[0x39] = 44, t[0x3A] = 57;
        for (int i = 0; i < 10; ++i)
            t[0x3B + i] = static_cast<std::uint16_t>(58 + i);
        t[0x45] = 83, t[0x46] = 71;
        t[0x47] = 95, t[0x48] = 96, t[0x49] = 97, t[0x4A] = 86;
        t[0x4B] = 92, t[0x4C] = 93, t[0x4D] = 94, t[0x4E] = 87;
        t[0x4F] = 89, t[0x50] = 90, t[0x51] = 91, t[0x52] = 98, t[0x53] = 99;
        t[0x56] = 100, t[0x57] = 68, t[0x58] = 69;
        t[0x9C] = 88, t[0x9D] = 228, t[0xB5] = 84, t[0xB7] = 70, t[0xB8] = 230;
        t[0xC5] = 72, t[0xC7] = 74, t[0xC8] = 82, t[0xC9] = 75, t[0xCB] = 80;
        t[0xCD] = 79, t[0xCF] = 77, t[0xD0] = 81, t[0xD1] = 78, t[0xD2] = 73;
        t[0xD3] = 76, t[0xDB] = 227, t[0xDC] = 231, t[0xDD] = 101;
        return t;
    }();

    constexpr std::uint32_t kDikEscape = 0x01;
    constexpr std::uint32_t kDikO = 0x18;   // Minecraft's pause / options menu (Esc stays MP2's)
    constexpr std::uint32_t kDikF5 = 0x3F;  // MP2 quicksave
    constexpr std::uint32_t kDikF9 = 0x43;  // MP2 quickload

    // DirectInput structures (the subset we read).
    struct DataFormat
    {
        DWORD dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs;
        void* rgodf;
    };
    struct MouseState
    {
        LONG lX, lY, lZ;
        BYTE rgbButtons[8];  // 4 for DIMOUSESTATE, 8 for DIMOUSESTATE2
    };
    struct ObjectData
    {
        DWORD dwOfs, dwData, dwTimeStamp, dwSequence;
    };
    constexpr DWORD kMouseOfsX = 0, kMouseOfsY = 4, kMouseOfsZ = 8, kMouseOfsButton0 = 12;

    enum class Kind
    {
        kUnknown,
        kKeyboard,
        kMouse,
    };

    using SetDataFormatFn = HRESULT(__stdcall*)(void*, const DataFormat*);
    using GetDeviceStateFn = HRESULT(__stdcall*)(void*, DWORD, void*);
    using GetDeviceDataFn = HRESULT(__stdcall*)(void*, DWORD, ObjectData*, DWORD*, DWORD);
    SetDataFormatFn  g_origSetDataFormat = nullptr;
    GetDeviceStateFn g_origGetDeviceState = nullptr;
    GetDeviceDataFn  g_origGetDeviceData = nullptr;

    std::mutex                       g_kindMutex;
    std::unordered_map<void*, Kind>  g_kinds;
    bool                             g_keyDown[256]{};
    bool                             g_buttonDown[8]{};
    ULONGLONG                        g_lastMouseStateMs = 0;
    WNDPROC                          g_origWndProc = nullptr;
    wchar_t                          g_highSurrogate = 0;

    Kind KindOf(void* device, DWORD stateBytes = 0)
    {
        std::lock_guard lock(g_kindMutex);
        auto            it = g_kinds.find(device);
        if (it != g_kinds.end() && it->second != Kind::kUnknown)
            return it->second;
        Kind kind = stateBytes == 256 ? Kind::kKeyboard : (stateBytes == 16 || stateBytes == 20) ? Kind::kMouse : Kind::kUnknown;
        if (kind != Kind::kUnknown)
            g_kinds[device] = kind;
        return kind;
    }

    bool Routed()
    {
        return !State().gameOwnsInput;
    }

    // Keys MP2 still sees while Minecraft drives Max (none while a Minecraft screen is open).
    bool GameKeeps(std::uint32_t dik)
    {
        if (State().mcScreenOpen)
            return false;
        return dik == kDikEscape || dik == kDikF9;
    }

    void HandleKey(std::uint32_t dik, bool down)
    {
        dik &= 0xFF;
        if (g_keyDown[dik] == down)
            return;
        g_keyDown[dik] = down;
        if (!Routed())
            return;

        auto&       st = State();
        auto&       link = Link::Get();
        const auto& cfg = Config::Get();
        if (!st.mcScreenOpen) {
            if (GameKeeps(dik) || dik == static_cast<std::uint32_t>(cfg.quickSaveKey) || dik == static_cast<std::uint32_t>(cfg.bulletTimeKey))
                return;  // MP2's (injected below)
            if (dik == kDikO) {
                if (down) {
                    input::ReleaseAll();
                    link.PushInput(proto::kInOpenMenu, 0);
                }
                return;
            }
        }
        if (const auto sdl = kDikToSdl[dik])
            link.PushInput(proto::kInKey, sdl, down ? 1 : 0);
    }

    void HandleButton(int index, bool down)
    {
        if (index < 0 || index >= 8 || g_buttonDown[index] == down)
            return;
        g_buttonDown[index] = down;
        if (!Routed())
            return;
        static constexpr std::uint16_t kSdl[8] = { 1, 3, 2, 4, 5, 0, 0, 0 };
        if (kSdl[index])
            Link::Get().PushInput(proto::kInMouseButton, kSdl[index], down ? 1 : 0);
    }

    void HandleMove(LONG dx, LONG dy, LONG wheel)
    {
        if (!Routed())
            return;
        auto& st = State();
        if (wheel)
            Link::Get().PushInput(proto::kInScroll, 0, wheel);
        if (!dx && !dy)
            return;
        if (st.mcScreenOpen) {
            const int x = std::clamp(st.cursorX.load() + static_cast<int>(dx), 0, st.viewportW.load() - 1);
            const int y = std::clamp(st.cursorY.load() + static_cast<int>(dy), 0, st.viewportH.load() - 1);
            st.cursorX = x;
            st.cursorY = y;
            Link::Get().PushInput(proto::kInCursor, 0, x, y);
        } else {
            st.lookDx += static_cast<int>(dx);
            st.lookDy += static_cast<int>(dy);
        }
    }

    HRESULT __stdcall SetDataFormat(void* self, const DataFormat* format)
    {
        if (format) {
            const Kind kind = format->dwDataSize == 256 ? Kind::kKeyboard : (format->dwDataSize == 16 || format->dwDataSize == 20) ? Kind::kMouse : Kind::kUnknown;
            std::lock_guard lock(g_kindMutex);
            g_kinds[self] = kind;
        }
        return g_origSetDataFormat(self, format);
    }

    HRESULT __stdcall GetDeviceState(void* self, DWORD bytes, void* data)
    {
        const HRESULT hr = g_origGetDeviceState(self, bytes, data);
        if (FAILED(hr) || !data)
            return hr;
        const auto& cfg = Config::Get();
        switch (KindOf(self, bytes)) {
        case Kind::kKeyboard:
            {
                auto* keys = static_cast<BYTE*>(data);
                for (std::uint32_t i = 0; i < (std::min)(bytes, 256ul); ++i)
                    HandleKey(i, (keys[i] & 0x80) != 0);
                if (Routed()) {
                    const bool quickSave = keys[cfg.quickSaveKey & 0xFF] & 0x80;
                    for (std::uint32_t i = 0; i < (std::min)(bytes, 256ul); ++i)
                        if (!GameKeeps(i))
                            keys[i] = 0;
                    if (quickSave && !State().mcScreenOpen)
                        keys[kDikF5] = 0x80;
                }
                break;
            }
        case Kind::kMouse:
            {
                auto*       m = static_cast<MouseState*>(data);
                const DWORD buttons = bytes >= 20 ? 8 : 4;
                g_lastMouseStateMs = GetTickCount64();
                HandleMove(m->lX, m->lY, m->lZ);
                for (DWORD i = 0; i < buttons; ++i)
                    HandleButton(static_cast<int>(i), (m->rgbButtons[i] & 0x80) != 0);
                if (Routed()) {
                    m->lX = m->lY = m->lZ = 0;
                    std::memset(m->rgbButtons, 0, buttons);
                    // B held: MP2's bullet time (its right mouse button).
                    if (g_keyDown[cfg.bulletTimeKey & 0xFF] && !State().mcScreenOpen)
                        m->rgbButtons[1] = 0x80;
                }
                break;
            }
        default:
            break;
        }
        return hr;
    }

    HRESULT __stdcall GetDeviceData(void* self, DWORD objectBytes, ObjectData* items, DWORD* inOut, DWORD flags)
    {
        const HRESULT hr = g_origGetDeviceData(self, objectBytes, items, inOut, flags);
        constexpr DWORD kPeek = 1;
        if (FAILED(hr) || !items || !inOut || (flags & kPeek) || objectBytes < sizeof(ObjectData))
            return hr;
        const Kind kind = KindOf(self);
        if (kind == Kind::kUnknown)
            return hr;
        const auto& cfg = Config::Get();
        auto*       bytes = reinterpret_cast<std::uint8_t*>(items);
        DWORD       kept = 0;
        for (DWORD i = 0; i < *inOut; ++i) {
            auto* item = reinterpret_cast<ObjectData*>(bytes + i * objectBytes);
            bool  keep = !Routed();
            if (kind == Kind::kKeyboard) {
                HandleKey(item->dwOfs, (item->dwData & 0x80) != 0);
                if (Routed()) {
                    keep = GameKeeps(item->dwOfs & 0xFF);
                    if ((item->dwOfs & 0xFF) == static_cast<DWORD>(cfg.quickSaveKey & 0xFF) && !State().mcScreenOpen) {
                        item->dwOfs = kDikF5;
                        keep = true;
                    }
                }
            } else {
                // Movement comes from GetDeviceState when the game polls that too.
                const bool polled = GetTickCount64() - g_lastMouseStateMs < 200;
                if (item->dwOfs == kMouseOfsX && !polled)
                    HandleMove(static_cast<LONG>(item->dwData), 0, 0);
                else if (item->dwOfs == kMouseOfsY && !polled)
                    HandleMove(0, static_cast<LONG>(item->dwData), 0);
                else if (item->dwOfs == kMouseOfsZ && !polled)
                    HandleMove(0, 0, static_cast<LONG>(item->dwData));
                else if (item->dwOfs >= kMouseOfsButton0 && item->dwOfs < kMouseOfsButton0 + 8)
                    HandleButton(static_cast<int>(item->dwOfs - kMouseOfsButton0), (item->dwData & 0x80) != 0);
            }
            if (keep) {
                if (kept != i)
                    std::memmove(bytes + kept * objectBytes, item, objectBytes);
                ++kept;
            }
        }
        *inOut = kept;
        return hr;
    }

    LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_CHAR && Routed() && State().mcScreenOpen) {
            const auto unit = static_cast<wchar_t>(wParam);
            if (unit >= 0xD800 && unit < 0xDC00) {
                g_highSurrogate = unit;
                return 0;
            }
            std::uint32_t code = unit;
            if (unit >= 0xDC00 && unit < 0xE000 && g_highSurrogate)
                code = 0x10000 + ((g_highSurrogate - 0xD800) << 10) + (unit - 0xDC00);
            g_highSurrogate = 0;
            if (code >= 32 && code != 127)
                Link::Get().PushInput(proto::kInText, 0, static_cast<std::int32_t>(code));
            return 0;
        }
        if (message == WM_KILLFOCUS)
            input::ReleaseAll();
        return CallWindowProcW(g_origWndProc, window, message, wParam, lParam);
    }
}

namespace
{
    // Hooked as the game creates them (no DirectInput objects of our own at startup).
    using DiCreateFn = HRESULT(__stdcall*)(HINSTANCE, DWORD, void**, void*);
    using DiCreateDeviceFn = HRESULT(__stdcall*)(void*, const GUID&, void**, void*);
    DiCreateFn       g_origDiCreate = nullptr;
    DiCreateDeviceFn g_origDiCreateDevice = nullptr;
    bool             g_deviceHooked = false;

    HRESULT __stdcall DiCreateDevice(void* di, const GUID& guid, void** out, void* outer)
    {
        const HRESULT hr = g_origDiCreateDevice(di, guid, out, outer);
        if (SUCCEEDED(hr) && out && *out && !g_deviceHooked) {
            g_deviceHooked = true;
            void** vtable = *static_cast<void***>(*out);
            InstallHook(vtable[9], reinterpret_cast<void*>(&GetDeviceState), g_origGetDeviceState, "IDirectInputDevice::GetDeviceState");
            InstallHook(vtable[10], reinterpret_cast<void*>(&GetDeviceData), g_origGetDeviceData, "IDirectInputDevice::GetDeviceData");
            InstallHook(vtable[11], reinterpret_cast<void*>(&SetDataFormat), g_origSetDataFormat, "IDirectInputDevice::SetDataFormat");
        }
        return hr;
    }

    HRESULT __stdcall DiCreate(HINSTANCE instance, DWORD version, void** out, void* outer)
    {
        const HRESULT hr = g_origDiCreate(instance, version, out, outer);
        if (SUCCEEDED(hr) && out && *out && !g_origDiCreateDevice)
            InstallHook((*static_cast<void***>(*out))[3], reinterpret_cast<void*>(&DiCreateDevice), g_origDiCreateDevice, "IDirectInput::CreateDevice");
        return hr;
    }
}

bool input::Install()
{
    HMODULE dinput = LoadLibraryW(L"dinput.dll");
    void*   create = dinput ? reinterpret_cast<void*>(GetProcAddress(dinput, "DirectInputCreateA")) : nullptr;
    if (!create) {
        mclog::Info("input: DirectInputCreateA not found");
        return false;
    }
    return InstallHook(create, reinterpret_cast<void*>(&DiCreate), g_origDiCreate, "DirectInputCreateA");
}

void input::AttachWindow(HWND window)
{
    if (g_origWndProc || !window)
        return;
    g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    mclog::Info("input: attached to window {:p}", static_cast<void*>(window));
}

void input::ReleaseAll()
{
    Link::Get().PushInput(proto::kInReleaseAll, 0);
}

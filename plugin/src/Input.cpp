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
    constexpr std::uint32_t kDikR = 0x13;  // MP2 reload

    // MP2 weapon mode: Minecraft keeps walking and looking; MP2 gets the mouse buttons, the wheel,
    // the number keys and R, so Max's own guns fire, switch and reload.
    std::atomic<bool> g_weaponMode{ false };

    bool WeaponKey(std::uint32_t dik)
    {
        return (dik >= 0x02 && dik <= 0x0A) || dik == kDikR;  // 1-9, R
    }
    constexpr std::uint32_t kDikE = 0x12;  // MP2's action key
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

    // Keyboard and mouse can be different classes with different vtables, so each vtable gets
    // patched (slots 9-11) with its own originals kept here.
    struct DeviceVtable
    {
        GetDeviceStateFn state;
        GetDeviceDataFn  data;
        SetDataFormatFn  format;
    };
    std::mutex                               g_vtableMutex;
    std::unordered_map<void**, DeviceVtable> g_vtables;

    DeviceVtable Originals(void* device)
    {
        std::lock_guard lock(g_vtableMutex);
        return g_vtables[*static_cast<void***>(device)];
    }

    // Mouse activity, for the log.
    std::atomic<int> g_mouseStateCalls{ 0 }, g_mouseDataItems{ 0 }, g_mouseMoved{ 0 };
    ULONGLONG        g_lastInputLog = 0;

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
        return dik == kDikEscape || dik == kDikF9 || (g_weaponMode && WeaponKey(dik));
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
            if (dik == static_cast<std::uint32_t>(cfg.weaponModeKey)) {
                if (down) {
                    g_weaponMode = !g_weaponMode;
                    input::ReleaseAll();  // Minecraft lets go of whatever it was doing with the mouse
                    mclog::Info("input: MP2 weapon mode {}", g_weaponMode ? "on (mouse buttons, wheel, 1-9, R -> MP2)" : "off");
                }
                return;
            }
            if (GameKeeps(dik) || dik == static_cast<std::uint32_t>(cfg.quickSaveKey) || dik == static_cast<std::uint32_t>(cfg.bulletTimeKey) ||
                dik == static_cast<std::uint32_t>(cfg.useKey))
                return;  // MP2's (passed through, or delivered as F5 / E below)
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
        if (!Routed() || (g_weaponMode && !State().mcScreenOpen))
            return;  // weapon mode: the buttons fire MP2's guns
        static constexpr std::uint16_t kSdl[8] = { 1, 3, 2, 4, 5, 0, 0, 0 };
        if (kSdl[index])
            Link::Get().PushInput(proto::kInMouseButton, kSdl[index], down ? 1 : 0);
    }

    void HandleMove(LONG dx, LONG dy, LONG wheel)
    {
        if (!Routed())
            return;
        auto& st = State();
        if (wheel && !(g_weaponMode && !st.mcScreenOpen))
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
        return Originals(self).format(self, format);
    }

    HRESULT __stdcall GetDeviceState(void* self, DWORD bytes, void* data)
    {
        const HRESULT hr = Originals(self).state(self, bytes, data);
        if (GetTickCount64() - g_lastInputLog > 5000) {
            g_lastInputLog = GetTickCount64();
            mclog::Info("input: mouse GetDeviceState {}x, buffered items {}, moved {}x, routed to Minecraft {}", g_mouseStateCalls.exchange(0),
                        g_mouseDataItems.exchange(0), g_mouseMoved.exchange(0), Routed());
        }
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
                    const bool use = keys[cfg.useKey & 0xFF] & 0x80;
                    for (std::uint32_t i = 0; i < (std::min)(bytes, 256ul); ++i)
                        if (!GameKeeps(i))
                            keys[i] = 0;
                    if (!State().mcScreenOpen) {
                        if (quickSave)
                            keys[kDikF5] = 0x80;
                        if (use)
                            keys[kDikE] = 0x80;
                    }
                }
                break;
            }
        case Kind::kMouse:
            {
                auto*       m = static_cast<MouseState*>(data);
                const DWORD buttons = bytes >= 20 ? 8 : 4;
                g_lastMouseStateMs = GetTickCount64();
                ++g_mouseStateCalls;
                if (m->lX || m->lY)
                    ++g_mouseMoved;
                HandleMove(m->lX, m->lY, m->lZ);
                for (DWORD i = 0; i < buttons; ++i)
                    HandleButton(static_cast<int>(i), (m->rgbButtons[i] & 0x80) != 0);
                if (Routed()) {
                    const bool weapons = g_weaponMode && !State().mcScreenOpen;
                    m->lX = m->lY = 0;
                    if (!weapons) {
                        m->lZ = 0;
                        std::memset(m->rgbButtons, 0, buttons);
                    }
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
        const HRESULT hr = Originals(self).data(self, objectBytes, items, inOut, flags);
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
                    } else if ((item->dwOfs & 0xFF) == static_cast<DWORD>(cfg.useKey & 0xFF) && !State().mcScreenOpen) {
                        item->dwOfs = kDikE;
                        keep = true;
                    }
                }
            } else {
                // Movement comes from GetDeviceState when the game polls that too.
                ++g_mouseDataItems;
                const bool polled = GetTickCount64() - g_lastMouseStateMs < 200;
                if (item->dwOfs == kMouseOfsX && !polled)
                    HandleMove(static_cast<LONG>(item->dwData), 0, 0);
                else if (item->dwOfs == kMouseOfsY && !polled)
                    HandleMove(0, static_cast<LONG>(item->dwData), 0);
                else if (item->dwOfs == kMouseOfsZ && !polled)
                    HandleMove(0, 0, static_cast<LONG>(item->dwData));
                else if (item->dwOfs >= kMouseOfsButton0 && item->dwOfs < kMouseOfsButton0 + 8)
                    HandleButton(static_cast<int>(item->dwOfs - kMouseOfsButton0), (item->dwData & 0x80) != 0);
                // Weapon mode: MP2 keeps its buttons and wheel (not the movement: the look is Minecraft's).
                if (Routed() && g_weaponMode && !State().mcScreenOpen && (item->dwOfs == kMouseOfsZ || item->dwOfs >= kMouseOfsButton0))
                    keep = true;
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

    bool GameKeepsVk(UINT vk);
    UINT QuickSaveVk();
    UINT GameVkFor(UINT vk);

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
        if ((message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && Routed()) {
            const UINT vk = static_cast<UINT>(wParam);
            if (const UINT game = GameVkFor(vk))
                return CallWindowProcW(g_origWndProc, window, message, game, lParam);
            if (!GameKeepsVk(vk))
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

    void PatchSlot(void** vtable, int slot, void* fn)
    {
        DWORD old = 0;
        if (VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
            vtable[slot] = fn;
            VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
        }
    }

    HRESULT __stdcall DiCreateDevice(void* di, const GUID& guid, void** out, void* outer)
    {
        const HRESULT hr = g_origDiCreateDevice(di, guid, out, outer);
        if (SUCCEEDED(hr) && out && *out) {
            void**          vtable = *static_cast<void***>(*out);
            std::lock_guard lock(g_vtableMutex);
            if (!g_vtables.contains(vtable) && vtable[9] != reinterpret_cast<void*>(&GetDeviceState)) {
                g_vtables[vtable] = { reinterpret_cast<GetDeviceStateFn>(vtable[9]), reinterpret_cast<GetDeviceDataFn>(vtable[10]),
                                      reinterpret_cast<SetDataFormatFn>(vtable[11]) };
                PatchSlot(vtable, 9, reinterpret_cast<void*>(&GetDeviceState));
                PatchSlot(vtable, 10, reinterpret_cast<void*>(&GetDeviceData));
                PatchSlot(vtable, 11, reinterpret_cast<void*>(&SetDataFormat));
                mclog::Info("input: patched device vtable {:p} ({} so far)", static_cast<void*>(vtable), g_vtables.size());
            }
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

namespace
{
    // Virtual-key versions of the keys MP2 keeps.
    UINT UseKeyVk()
    {
        static const UINT vk = MapVirtualKeyW(static_cast<UINT>(Config::Get().useKey) & 0xFF, MAPVK_VSC_TO_VK);
        return vk;
    }

    bool GameKeepsVk(UINT vk)
    {
        if (State().mcScreenOpen)
            return false;
        return vk == VK_ESCAPE || vk == VK_F9 || (g_weaponMode && ((vk >= '1' && vk <= '9') || vk == 'R'));
    }

    UINT QuickSaveVk()
    {
        static const UINT vk = MapVirtualKeyW(static_cast<UINT>(Config::Get().quickSaveKey) & 0xFF, MAPVK_VSC_TO_VK);
        return vk;
    }

    using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
    using GetKeyStateFn = SHORT(WINAPI*)(int);
    using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
    using SetWindowsHookExAFn = HHOOK(WINAPI*)(int, HOOKPROC, HINSTANCE, DWORD);
    using TranslateAcceleratorAFn = int(WINAPI*)(HWND, HACCEL, LPMSG);
    GetAsyncKeyStateFn      g_origGetAsyncKeyState = nullptr;
    GetKeyStateFn           g_origGetKeyState = nullptr;
    GetKeyboardStateFn      g_origGetKeyboardState = nullptr;
    SetWindowsHookExAFn     g_origSetWindowsHookExA = nullptr;
    TranslateAcceleratorAFn g_origTranslateAcceleratorA = nullptr;
    HOOKPROC                g_gameKeyboardHook = nullptr;

    bool QuickSaveHeld()
    {
        const UINT vk = QuickSaveVk();
        return vk && (g_origGetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    }

    bool UseHeld()
    {
        const UINT vk = UseKeyVk();
        return vk && (g_origGetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    }

    // While Minecraft has the keyboard, the key MP2 should get for physical key `vk`: our quicksave
    // key arrives as F5, our action key as E; 0 for keys MP2 doesn't get at all.
    UINT GameVkFor(UINT vk)
    {
        if (State().mcScreenOpen)
            return 0;
        if (vk == QuickSaveVk())
            return VK_F5;
        if (vk == UseKeyVk())
            return 'E';
        return 0;
    }

    // Whether MP2 should read virtual key `vk` as held because its stand-in key is.
    bool InjectedDown(int vk)
    {
        if (State().mcScreenOpen)
            return false;
        return (vk == VK_F5 && QuickSaveHeld()) || (vk == 'E' && UseHeld());
    }

    // What MP2 may see of key `vk` while Minecraft has the keyboard.
    bool Hidden(int vk)
    {
        return Routed() && !GameKeepsVk(static_cast<UINT>(vk) & 0xFF);
    }

    SHORT WINAPI GetAsyncKeyStateHook(int vk)
    {
        if (Hidden(vk))
            return InjectedDown(vk) ? SHORT(0x8000) : SHORT(0);
        return g_origGetAsyncKeyState(vk);
    }

    SHORT WINAPI GetKeyStateHook(int vk)
    {
        if (Hidden(vk))
            return InjectedDown(vk) ? SHORT(0xFF80) : SHORT(0);
        return g_origGetKeyState(vk);
    }

    BOOL WINAPI GetKeyboardStateHook(PBYTE keys)
    {
        const BOOL ok = g_origGetKeyboardState(keys);
        if (ok && keys && Routed()) {
            for (int vk = 0; vk < 256; ++vk)
                if (!GameKeepsVk(static_cast<UINT>(vk)))
                    keys[vk] = 0;
            if (InjectedDown(VK_F5))
                keys[VK_F5] = 0x80;
            if (InjectedDown('E'))
                keys['E'] = 0x80;
        }
        return ok;
    }

    // MP2's WH_KEYBOARD hook: while Minecraft has the keyboard it only hears the keys MP2 keeps
    // (and our quicksave key, as F5).
    LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code >= 0 && Routed() && g_gameKeyboardHook) {
            const UINT vk = static_cast<UINT>(wParam);
            if (const UINT game = GameVkFor(vk))
                return g_gameKeyboardHook(code, game, lParam);
            if (!GameKeepsVk(vk))
                return CallNextHookEx(nullptr, code, wParam, lParam);
        }
        return g_gameKeyboardHook ? g_gameKeyboardHook(code, wParam, lParam) : CallNextHookEx(nullptr, code, wParam, lParam);
    }

    HHOOK WINAPI SetWindowsHookExAHook(int id, HOOKPROC proc, HINSTANCE module, DWORD thread)
    {
        mclog::Info("input: MP2 installs a Windows hook (type {})", id);
        if ((id == WH_KEYBOARD || id == WH_KEYBOARD_LL) && proc && !g_gameKeyboardHook) {
            g_gameKeyboardHook = proc;
            return g_origSetWindowsHookExA(id, KeyboardHookProc, module, thread);
        }
        return g_origSetWindowsHookExA(id, proc, module, thread);
    }

    // Keyboard shortcuts (quicksave and friends): only for the keys MP2 keeps.
    int WINAPI TranslateAcceleratorAHook(HWND window, HACCEL table, LPMSG message)
    {
        if (message && Routed() &&
            (message->message == WM_KEYDOWN || message->message == WM_KEYUP || message->message == WM_SYSKEYDOWN || message->message == WM_SYSKEYUP)) {
            const UINT vk = static_cast<UINT>(message->wParam);
            if (const UINT game = GameVkFor(vk)) {
                MSG copy = *message;
                copy.wParam = game;
                return g_origTranslateAcceleratorA(window, table, &copy);
            }
            if (!GameKeepsVk(vk))
                return 0;
        }
        return g_origTranslateAcceleratorA(window, table, message);
    }

    void HookWindowsKeyboard()
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto    at = [&](const char* name) { return reinterpret_cast<void*>(GetProcAddress(user32, name)); };
        InstallHook(at("GetAsyncKeyState"), reinterpret_cast<void*>(&GetAsyncKeyStateHook), g_origGetAsyncKeyState, "GetAsyncKeyState");
        InstallHook(at("GetKeyState"), reinterpret_cast<void*>(&GetKeyStateHook), g_origGetKeyState, "GetKeyState");
        InstallHook(at("GetKeyboardState"), reinterpret_cast<void*>(&GetKeyboardStateHook), g_origGetKeyboardState, "GetKeyboardState");
        InstallHook(at("SetWindowsHookExA"), reinterpret_cast<void*>(&SetWindowsHookExAHook), g_origSetWindowsHookExA, "SetWindowsHookExA");
        InstallHook(at("TranslateAcceleratorA"), reinterpret_cast<void*>(&TranslateAcceleratorAHook), g_origTranslateAcceleratorA, "TranslateAcceleratorA");
    }
}

bool input::Install()
{
    HookWindowsKeyboard();
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

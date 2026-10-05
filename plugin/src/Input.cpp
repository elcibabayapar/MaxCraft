#include "Input.h"

#include "Config.h"
#include "Hook.h"
#include "Log.h"
#include "Runtime.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
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

    // ---- The routing table: which keys MP2 keeps, and under which name --------------------------
    //
    // Max Payne 2 reads the keyboard through six independent paths, and every one of them has to
    // hide the keys MaxCraft gives to Minecraft and rewrite the ones MaxCraft remaps:
    //
    //   DirectInput GetDeviceData     buffered, items carrying a dwOfs scan-code offset
    //   DirectInput GetDeviceState    polled, a 256-entry scan-code state array
    //   GetAsyncKeyState              per key
    //   GetKeyState                   per key
    //   GetKeyboardState              a 256-entry virtual-key state array
    //   SetWindowsHookExA keyboard hook, the window procedure, TranslateAcceleratorA
    //
    // The answer is given once, below: the rows of kRows, the weapon-mode rule and the MaxCraft.ini
    // keys form one table, and every path reads it through RouteScan(), RouteVk() or FilterState().
    // A path may only translate keys between its own representation (scan code, virtual key, buffer
    // offset) and this table. It must not decide anything itself.
    //
    // WARNING: a path that filters or rewrites on its own reintroduces the F5 leak. Scan code 0x3F
    // (F5) is Minecraft's camera key and must never reach MP2, while the quicksave key (F6 by
    // default) has to arrive at MP2 *as* F5, because that is the binding MP2's quicksave is on. F5
    // leaked out through TranslateAcceleratorA for exactly this reason: that path carried its own
    // copy of the rules, and adding a remap or a path only had to be forgotten in one place.

    // MP2's own keys that MaxCraft stands in for, as scan codes. Hardcoded because they are MP2's
    // bindings, not MaxCraft's: nothing in MaxCraft.ini may move them.
    constexpr std::uint32_t kGameQuickSave = 0x3F;  // DIK_F5: MP2's quicksave slot
    constexpr std::uint32_t kGameAction    = 0x12;  // DIK_E: MP2's action key (doors, switches)
    constexpr std::uint32_t kDikEscape     = 0x01;
    constexpr std::uint32_t kDikR          = 0x13;  // MP2 reload
    constexpr std::uint32_t kDikO          = 0x18;  // Minecraft's pause / options menu (Esc is MP2's)
    constexpr std::uint32_t kDikF9         = 0x43;  // MP2 quickload

    // MP2 weapon mode: Minecraft keeps walking and looking; MP2 gets the mouse buttons, the wheel,
    // the number keys and R, so Max's own guns fire, switch and reload.
    std::atomic<bool> g_weaponMode{ false };

    bool WeaponsToGame()
    {
        return g_weaponMode && !State().mcScreenOpen;
    }

    // What happens to a physical key while Minecraft drives Max.
    enum class Where
    {
        kMinecraft,   // forwarded to Minecraft; MP2 must not see it at all
        kGame,        // MP2 reads it unchanged; MaxCraft holds it back from Minecraft
        kGameAs,      // MP2 reads it as `as` instead of the pressed scan code (our remaps)
        kGameWeapons, // MP2's while weapon mode is on (WeaponKey below)
        kConsumed,    // MaxCraft's own: the weapon-mode toggle, B (MP2's right mouse button), O
    };

    struct Route
    {
        Where         where{ Where::kMinecraft };
        std::uint32_t as{ 0 };
    };

    // MP2's own weapon keys: 1-9 select its weapon slots and R reloads. They are a rule rather than
    // rows because they belong to MP2 only while weapon mode is on, and MaxCraft hands them over only
    // so that Max can shoot: the look stays Minecraft's.
    bool WeaponKey(std::uint32_t dik)
    {
        return (dik >= 0x02 && dik <= 0x0A) || dik == kDikR;  // 1-9, R
    }

    // The hardcoded rows of the table. Why each one cannot be an ini option is in the comment; a
    // hardcoded binding is exactly one row here.
    struct Row
    {
        std::uint32_t dik;  // the pressed scan code
        Where         where;
        std::uint32_t as;   // for kGameAs: the scan code MP2 reads instead
    };
    constexpr Row kRows[] = {
        { kDikEscape, Where::kGame, 0 },  // MP2's menu, and the only way out of a level
        { kDikF9, Where::kGame, 0 },      // MP2 quickload
        { kDikO, Where::kConsumed, 0 },   // MP2 has no pause menu that can work while Minecraft drives
                                         // Max, so O opens Minecraft's instead (Esc stays MP2's)
    };

    // The table itself: the hardcoded rows, the weapon-mode rule and the four ini keys. The ini keys
    // are checked first, so MaxCraft.ini can take any hardcoded binding over; there is no ini key for
    // a binding that must stay MP2's own.
    Route StaticRoute(std::uint32_t dik)
    {
        const auto& cfg = Config::Get();
        if (dik == static_cast<std::uint32_t>(cfg.quickSaveKey))
            return { Where::kGameAs, kGameQuickSave };  // F6 -> MP2's F5, because F5 is Minecraft's camera key
        if (dik == static_cast<std::uint32_t>(cfg.useKey))
            return { Where::kGameAs, kGameAction };  // G -> MP2's E, because E is Minecraft's inventory key
        if (dik == static_cast<std::uint32_t>(cfg.weaponModeKey))
            return { Where::kConsumed, 0 };
        if (dik == static_cast<std::uint32_t>(cfg.bulletTimeKey))
            return { Where::kConsumed, 0 };  // we deliver it as MP2's right mouse button
        for (const Row& row : kRows)
            if (dik == row.dik)
                return { row.where, row.as };
        if (WeaponKey(dik))
            return { Where::kGameWeapons, 0 };
        return {};
    }

    // Two rules are not in the table because they change while the game runs, so every path applies
    // them through here: a Minecraft screen open (chat, inventory, options) makes every key
    // Minecraft's - typing has to work and Esc closes the screen - and the weapon keys are MP2's
    // only while weapon mode is on.
    Route Applied(Route route)
    {
        if (State().mcScreenOpen || (route.where == Where::kGameWeapons && !g_weaponMode))
            return {};
        return route;
    }

    // Does MP2 read this key itself, unchanged? (A kGameAs key is MP2's too, but under another name:
    // FilterState delivers it, GetDeviceData rewrites the buffer item.)
    //
    // kGameWeapons counts as MP2's. Applied() has already turned it into kMinecraft when weapon mode
    // is off, so a route that is still kGameWeapons here means weapon mode is on and these are Max's
    // own weapon keys (1-9, R). Testing only for kGame would hide them from every path at once - the
    // buffered items, the polled state array, GetAsyncKeyState, GetKeyState, GetKeyboardState - and
    // weapon mode would silently do nothing.
    bool GameKeeps(const Route& route)
    {
        return route.where == Where::kGame || route.where == Where::kGameWeapons;
    }

    // The DirectInput paths (scan codes).
    Route RouteScan(std::uint32_t dik)
    {
        return Applied(StaticRoute(dik & 0xFF));
    }

    // The Windows paths speak virtual-key codes. This is the same table keyed by virtual key, built
    // from it once, so the two halves cannot drift apart.
    Route RouteVk(UINT vk)
    {
        static const std::array<Route, 256> byVk = [] {
            std::array<Route, 256> t{};
            for (std::uint32_t dik = 0; dik < 256; ++dik) {
                const Route route = StaticRoute(dik);
                if (route.where == Where::kMinecraft)
                    continue;  // no route: the key is Minecraft's
                if (const UINT vk = MapVirtualKeyW(dik, MAPVK_VSC_TO_VK))
                    t[vk] = route;
            }
            return t;
        }();
        return Applied(byVk[vk & 0xFF]);
    }

    UINT VkOfScan(std::uint32_t dik)
    {
        return MapVirtualKeyW(dik & 0xFF, MAPVK_VSC_TO_VK);
    }

    // The remaps, in whatever key representation the caller speaks: what the user pressed, and the key
    // MP2 reads it as. Found by asking the table which keys it rewrites, so a third remap added to
    // the table needs no change here.
    struct Remap
    {
        std::uint32_t from;
        std::uint32_t to;
    };
    struct Remaps
    {
        std::array<Remap, 4> list{};
        std::size_t         count{ 0 };
    };

    const Remaps& ScanRemaps()
    {
        static const Remaps remaps = [] {
            Remaps out;
            for (std::uint32_t dik = 0; dik < 256; ++dik)
                if (const Route route = StaticRoute(dik); route.where == Where::kGameAs)
                    out.list[out.count++] = { dik, route.as };
            return out;
        }();
        return remaps;
    }

    const Remaps& VkRemaps()
    {
        static const Remaps remaps = [] {
            Remaps out;
            for (std::uint32_t dik = 0; dik < 256; ++dik)
                if (const Route route = StaticRoute(dik); route.where == Where::kGameAs)
                    out.list[out.count++] = { VkOfScan(dik), VkOfScan(route.as) };
            return out;
        }();
        return remaps;
    }

    // The whole-state rewrite, shared by the two paths that hand MP2 a full keyboard state array
    // (DirectInput's scan-code array and Windows' virtual-key array). `route` and `remaps` are in the
    // caller's own key representation, so there is one rewrite and not one per path.
    template <class RouteFn>
    void FilterState(BYTE* keys, DWORD count, RouteFn route, const Remaps& remaps)
    {
        // Everything that is not MP2's leaves the array. Our own remapped keys stay for the second
        // pass, which needs their bits, but MP2 must not see them under the name the user pressed.
        for (std::uint32_t i = 0; i < count; ++i) {
            const Route key = route(i);
            if (!GameKeeps(key) && key.where != Where::kGameAs)
                keys[i] = 0;
        }
        // Pass two: a remapped key arrives under MP2's name only (F6 as F5, G as E).
        for (std::size_t r = 0; r < remaps.count; ++r) {
            const Remap remap = remaps.list[r];
            if (remap.from >= count || remap.to >= count || !(keys[remap.from] & 0x80))
                continue;
            keys[remap.to] = 0x80;
            keys[remap.from] = 0;
        }
    }

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
    std::atomic<int> g_mouseStateCalls{ 0 }, g_mouseDataItems{ 0 };
    std::atomic<int> g_mouseMoved{ 0 }, g_mouseSkipped{ 0 };
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

    void HandleKey(std::uint32_t dik, bool down)
    {
        dik &= 0xFF;
        if (g_keyDown[dik] == down)
            return;
        g_keyDown[dik] = down;
        if (!Routed())
            return;

        auto&       link = Link::Get();
        const auto& cfg = Config::Get();
        switch (RouteScan(dik).where) {
        case Where::kConsumed:
            // MaxCraft's own keys: neither Minecraft nor MP2 sees them under their own name.
            if (!down)
                return;
            if (dik == static_cast<std::uint32_t>(cfg.weaponModeKey)) {
                g_weaponMode = !g_weaponMode;
                input::ReleaseAll();  // Minecraft lets go of whatever it was doing with the mouse
                mclog::Info("input: MP2 weapon mode {}", g_weaponMode ? "on (mouse buttons, wheel, 1-9, R -> MP2)" : "off");
            } else if (dik == kDikO) {
                input::ReleaseAll();
                link.PushInput(proto::kInOpenMenu, 0);
            }
            // cfg.bulletTimeKey does nothing here: the mouse path delivers it as MP2's right button
            // while it is held, so MP2 never reads the key itself.
            return;
        case Where::kGame:
        case Where::kGameAs:
            return;  // MP2's: passed through, or delivered as F5 / E by the path MP2 reads it through
        case Where::kGameWeapons:
            // MP2's alone. Weapon mode hands 1-9 and R to Max's own guns on purpose, and Minecraft
            // must not also act on them - otherwise every shot would drag Minecraft's hotbar
            // selection along with it, which is the one thing weapon mode is meant to stop. Applied()
            // has already turned this into kMinecraft when weapon mode is off, so reaching the case
            // means weapon mode is on and the key really is MP2's.
            return;
        case Where::kMinecraft:
            break;  // Minecraft's: forwarded below
        }
        if (const auto sdl = kDikToSdl[dik])
            link.PushInput(proto::kInKey, sdl, down ? 1 : 0);
    }

    void HandleButton(int index, bool down)
    {
        if (index < 0 || index >= 8 || g_buttonDown[index] == down)
            return;
        g_buttonDown[index] = down;
        if (!Routed() || WeaponsToGame())
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
        if (wheel && !WeaponsToGame())
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

    // Everything we think is held, so Minecraft's own state and ours cannot disagree about it.
    void ClearHeld()
    {
        for (bool& down : g_keyDown)
            down = false;
        for (bool& down : g_buttonDown)
            down = false;
        g_highSurrogate = 0;
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
            mclog::Info("input: mouse GetDeviceState {}x, buffered items {}, moved {}x, buffered motion deduped {}x, routed to Minecraft {}",
                        g_mouseStateCalls.exchange(0), g_mouseDataItems.exchange(0), g_mouseMoved.exchange(0), g_mouseSkipped.exchange(0), Routed());
        }
        if (FAILED(hr) || !data)
            return hr;
        switch (KindOf(self, bytes)) {
        case Kind::kKeyboard:
            {
                auto*       keys = static_cast<BYTE*>(data);
                const DWORD count = (std::min)(bytes, 256ul);
                for (std::uint32_t i = 0; i < count; ++i)
                    HandleKey(i, (keys[i] & 0x80) != 0);
                if (Routed())
                    FilterState(keys, count, RouteScan, ScanRemaps());
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
                    const bool weapons = WeaponsToGame();
                    m->lX = m->lY = 0;
                    if (!weapons) {
                        m->lZ = 0;
                        std::memset(m->rgbButtons, 0, buttons);
                    }
                    // The bullet-time key held: MP2's bullet time, which is its right mouse button.
                    // The key itself is kConsumed in the table, so this is the only way MP2 gets it.
                    if (g_keyDown[static_cast<std::uint32_t>(Config::Get().bulletTimeKey) & 0xFF] && !State().mcScreenOpen)
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
        auto*       bytes = reinterpret_cast<std::uint8_t*>(items);
        DWORD       kept = 0;
        // MP2 reads the mouse both ways when it polls GetDeviceState as well: that read reports the
        // motion accumulated since the previous one, and the buffer replays the same motion. The
        // polled read is authoritative while it keeps coming (16 ms at 60 fps), so buffered motion
        // that soon after one is dropped. A device timestamp (dwTimeStamp) would say exactly whether
        // the polled read already covered an item, but GetDeviceState reports no timestamp, so there
        // is nothing cheaper than this window. Its cost: if the game stops polling GetDeviceState for
        // more than 200 ms, the first 200 ms of motion after its last poll is lost (the mouse stops,
        // then catches up). Lower the window and a fast flick can be counted twice; raise it and the
        // gap after a pause grows. g_mouseSkipped counts what was dropped.
        const bool polled = kind == Kind::kMouse && GetTickCount64() - g_lastMouseStateMs < 200;
        for (DWORD i = 0; i < *inOut; ++i) {
            auto* item = reinterpret_cast<ObjectData*>(bytes + i * objectBytes);
            bool  keep = !Routed();
            if (kind == Kind::kKeyboard) {
                HandleKey(item->dwOfs, (item->dwData & 0x80) != 0);
                if (Routed()) {
                    const Route route = RouteScan(item->dwOfs);
                    keep = GameKeeps(route);
                    if (route.where == Where::kGameAs && route.as != (item->dwOfs & 0xFF)) {
                        item->dwOfs = route.as;  // our quicksave/use key arrives as F5/E
                        keep = true;
                    }
                }
            } else {
                ++g_mouseDataItems;
                const bool motion = item->dwOfs == kMouseOfsX || item->dwOfs == kMouseOfsY || item->dwOfs == kMouseOfsZ;
                if (motion && polled)
                    ++g_mouseSkipped;
                else if (item->dwOfs == kMouseOfsX)
                    HandleMove(static_cast<LONG>(item->dwData), 0, 0);
                else if (item->dwOfs == kMouseOfsY)
                    HandleMove(0, static_cast<LONG>(item->dwData), 0);
                else if (item->dwOfs == kMouseOfsZ)
                    HandleMove(0, 0, static_cast<LONG>(item->dwData));
                else if (item->dwOfs >= kMouseOfsButton0 && item->dwOfs < kMouseOfsButton0 + 8)
                    HandleButton(static_cast<int>(item->dwOfs - kMouseOfsButton0), (item->dwData & 0x80) != 0);
                // Weapon mode: MP2 keeps its buttons and wheel (not the movement: the look is Minecraft's).
                if (Routed() && WeaponsToGame() && (item->dwOfs == kMouseOfsZ || item->dwOfs >= kMouseOfsButton0))
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
    UINT GameVkFor(UINT vk);
    bool InjectedDown(UINT vk);

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

    // The real pressed state of a key, read through the unhooked call: the hook itself would filter
    // it, and MP2 must see the stand-in key's state on the target key.
    bool Held(UINT vk)
    {
        return vk && (g_origGetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
    }

    // Whether MP2 sees virtual key `vk` at all. Asks GameKeeps rather than repeating its test, so
    // the Windows half cannot disagree with the DirectInput half about weapon mode's 1-9 and R.
    bool GameKeepsVk(UINT vk)
    {
        return GameKeeps(RouteVk(vk));
    }

    // While Minecraft has the keyboard, the key MP2 should get for physical key `vk`: our quicksave
    // key arrives as F5, our action key as E; 0 for keys MP2 does not get at all.
    UINT GameVkFor(UINT vk)
    {
        const Route route = RouteVk(vk);
        return route.where == Where::kGameAs ? VkOfScan(route.as) : 0;
    }

    // Whether MP2 should read virtual key `vk` as held because the key it stands for is held. (With a
    // Minecraft screen open RouteVk() withdraws the remap, so nothing is injected.)
    bool InjectedDown(UINT vk)
    {
        const Remaps& remaps = VkRemaps();
        for (std::size_t r = 0; r < remaps.count; ++r)
            if (remaps.list[r].to == vk && RouteVk(remaps.list[r].from).where == Where::kGameAs)
                return Held(remaps.list[r].from);
        return false;
    }

    // What MP2 may see of key `vk` while Minecraft has the keyboard: nothing, unless the table keeps
    // it or a remap delivers it under another key.
    bool Hidden(int vk)
    {
        return Routed() && !GameKeeps(RouteVk(static_cast<UINT>(vk)));
    }

    SHORT WINAPI GetAsyncKeyStateHook(int vk)
    {
        if (Hidden(vk))
            return InjectedDown(static_cast<UINT>(vk)) ? SHORT(0x8000) : SHORT(0);
        return g_origGetAsyncKeyState(vk);
    }

    SHORT WINAPI GetKeyStateHook(int vk)
    {
        if (Hidden(vk))
            return InjectedDown(static_cast<UINT>(vk)) ? SHORT(0xFF80) : SHORT(0);
        return g_origGetKeyState(vk);
    }

    BOOL WINAPI GetKeyboardStateHook(PBYTE keys)
    {
        const BOOL ok = g_origGetKeyboardState(keys);
        if (ok && keys && Routed())
            FilterState(keys, 256, RouteVk, VkRemaps());
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

    // Keyboard shortcuts (quicksave and friends): only for the keys MP2 keeps. F5 used to leak out of
    // MP2's own accelerator this way, before the table existed; this path asks the table like the rest.
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

    // A name for a scan code, so the routing line below can be read without the DirectInput table: the
    // number row, the function keys and the few letter keys MaxCraft's own bindings use. Anything else
    // prints as the hex scan code MaxCraft.ini uses.
    std::string ScanName(std::uint32_t dik)
    {
        dik &= 0xFF;
        if (dik >= 0x02 && dik <= 0x0A)
            return std::string(1, static_cast<char>('1' + dik - 0x02));  // 1-9, MP2's weapon slots
        if (dik >= 0x3B && dik <= 0x44)
            return "F" + std::to_string(dik - 0x3A);
        switch (dik) {
        case kDikEscape: return "Esc";
        case 0x0B: return "0";
        case kGameAction: return "E";
        case kDikR: return "R";
        case kDikO: return "O";
        case 0x22: return "G";
        case 0x2F: return "V";
        case 0x30: return "B";
        default: return std::format("0x{:02x}", dik);
        }
    }

    // The effective routing, once at startup: which keys MP2 keeps and what the two remaps deliver,
    // so a user can see what MaxCraft believes its bindings are instead of guessing from a symptom.
    void LogRouting()
    {
        const auto& cfg = Config::Get();
        mclog::Info("input: routing: MP2 keeps Esc (0x01), F9 (0x43) and 1-9/R in weapon mode; quicksave {} -> MP2's {}, use {} -> MP2's {}, "
                    "{} toggles weapon mode, {} held is bullet time, O opens Minecraft's pause menu; every other key is Minecraft's",
                    ScanName(cfg.quickSaveKey), ScanName(kGameQuickSave), ScanName(cfg.useKey), ScanName(kGameAction), ScanName(cfg.weaponModeKey),
                    ScanName(cfg.bulletTimeKey));
    }
}

bool input::Install()
{
    HookWindowsKeyboard();
    LogRouting();
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
    if (!window)
        return;
    // The Present hook calls this every frame, and the game re-creates its window on a resolution
    // change or a device reset: the subclass has to follow it. Only one previous procedure is kept,
    // which is the one of the window being subclassed now.
    if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC)) == &WndProc)
        return;
    const WNDPROC previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    if (!previous) {
        mclog::Info("input: could not subclass window {:p}", static_cast<void*>(window));
        return;
    }
    g_origWndProc = previous;
    mclog::Info("input: attached to window {:p}", static_cast<void*>(window));
}

void input::ReleaseAll()
{
    // Our own view of what is held has to go with Minecraft's: a key that was down when MP2 took Max
    // back stays "down" here, and the next press edge for it never comes, so it would stay dead in
    // Minecraft until it is pressed again. Also drops a half-typed surrogate pair (text input).
    ClearHeld();
    Link::Get().PushInput(proto::kInReleaseAll, 0);
}

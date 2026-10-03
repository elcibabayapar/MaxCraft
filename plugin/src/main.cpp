#include "Camera.h"
#include "Characters.h"
#include "Collision.h"
#include "CrashLog.h"
#include "Config.h"
#include "Engine.h"
#include "Input.h"
#include "Launcher.h"
#include "Link.h"
#include "Log.h"
#include "Render.h"

#include <MinHook.h>
#include <Windows.h>

namespace
{
    DWORD WINAPI Startup(LPVOID)
    {
        mclog::Init();
        crashlog::Install();
        mclog::Info("MaxCraft 0.1.1 loaded (protocol v{}, built {} {})", proto::kVersion, __DATE__, __TIME__);

        if (!mp2::Resolve()) {
            mclog::Info("unsupported Max Payne 2 version: MaxCraft stays inactive");
            return 0;
        }
        // The shared memory (~120 MB of views) is mapped at MP2's first frame, not here: taking that
        // much of a 32-bit address space while MP2 makes its own big startup allocations made one of
        // them fail (crash reading address 0 at maxpayne2.exe+0x1b80, on some launches).
        if (MH_Initialize() != MH_OK) {
            mclog::Info("MinHook failed to initialize");
            return 0;
        }

        bool ok = true;
        ok &= collision::Install();  // first: rooms are built while the first level loads
        ok &= characters::Install();
        ok &= camera::Install();
        ok &= input::Install();
        ok &= render::Install();
        if (ok)
            mclog::Info("MaxCraft ready");
        else
            mclog::Info("MaxCraft running with missing parts (see above)");
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Engine DLLs are static imports of maxpayne2.exe, so they are mapped already; the hooks go
        // in from a thread because DllMain runs under the loader lock.
        if (HANDLE thread = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr))
            CloseHandle(thread);
    }
    return TRUE;
}

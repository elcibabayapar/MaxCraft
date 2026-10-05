#include "Camera.h"
#include "Characters.h"
#include "Collision.h"
#include "CrashLog.h"
#include "Config.h"
#include "Engine.h"
#include "Hook.h"
#include "Input.h"
#include "Launcher.h"
#include "Link.h"
#include "Log.h"
#include "Render.h"

#include <MinHook.h>
#include <Windows.h>

// plugin/CMakeLists.txt has the one copy of this and passes it in; the fallback keeps a plain
// compiler command honest. The log's first line is how the user tells which build is installed.
#ifndef MCRAFT_VERSION
#define MCRAFT_VERSION "0.1.1"
#endif

namespace
{
    DWORD WINAPI Startup(LPVOID)
    {
        mclog::Init();
        crashlog::Install();
        mclog::Info("MaxCraft {} loaded (protocol v{}, built {} {})", MCRAFT_VERSION, proto::kVersion, __DATE__, __TIME__);

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

        // One batch for all of them: MinHook suspends every thread per MH_EnableHook (~44 ms here),
        // so seventeen installs cost ~0.75 s during which MP2's own window procedure can run into a
        // half-patched hook table - the leading suspect for maxpayne2.exe+0x1b80 in docs/HANDOFF.md
        // §8.1. One flush does the same work with a single suspension. This narrows the window; it
        // does not prove anything, and the user has to say whether the crash is gone.
        hooks::BeginBatch();
        bool ok = true;
        ok &= collision::Install();  // first: rooms are built while the first level loads
        ok &= characters::Install();
        ok &= camera::Install();
        ok &= input::Install();
        ok &= render::Install();
        ok &= hooks::FlushHooks();
        if (ok)
            mclog::Info("MaxCraft ready");
        else
            mclog::Info("MaxCraft running with missing parts (see above)");
        return 0;
    }

    // What DLL_PROCESS_DETACH can and cannot do, in one place. Both paths run under the loader lock,
    // where joining a thread, taking a lock and suspending anything are all off the table: a thread
    // suspended by MinHook while it waits for the loader lock would never be resumed.
    //
    // lpReserved != nullptr: the process is terminating. ExitProcess has already stopped every thread
    // but this one, so no producer is inside a view and this is the one unload path where the
    // teardown order can actually be right. Unmapping the shared memory is not what makes the exit
    // clean - the address space is discarded whole - it is here because Link::Shutdown() is
    // idempotent and this exercises the same path a clean shutdown would.
    //
    // lpReserved == nullptr: FreeLibrary. Deliberately nothing. Two detached threads of ours are
    // still running code that is about to be unmapped - Collision.cpp's worker and Launcher's - and
    // nothing legal under the loader lock can stop them. Unlinking the shared memory here would only
    // decide where the inevitable access violation lands, so the honest answer is that MaxCraft.asi
    // cannot be unloaded while the game runs; the game has to exit.
    void OnDetach(LPVOID reserved)
    {
        if (!reserved)
            return;
        // Stop the producer before the mapping goes: the collision worker is the one thread that can
        // be inside Link::WriteCollision() holding a pointer into the views, and unmapViewOfFile
        // under it would fault in a thread we are already tearing down. Safe here and only here,
        // because process termination has stopped every other thread already - the join returns at
        // once instead of waiting for a worker that is not running any more.
        collision::StopWorker();
        Link::Shutdown();
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        // Engine DLLs are static imports of maxpayne2.exe, so they are mapped already; the hooks go
        // in from a thread because DllMain runs under the loader lock.
        if (HANDLE thread = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr))
            CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        OnDetach(reserved);
    }
    return TRUE;
}

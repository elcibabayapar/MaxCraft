#pragma once

#include "Log.h"

#include <MinHook.h>

// Detours `target` to `detour` and stores the trampoline in `original`.
// MSVC can't put __thiscall on free functions, so detours of member functions are written as
// __fastcall(self, edx-dummy, args...): `this` arrives in ECX either way.
template <class T>
bool InstallHook(void* target, void* detour, T& original, const char* name)
{
    if (!target) {
        mclog::Info("hook {}: no target", name);
        return false;
    }
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) != MH_OK || MH_EnableHook(target) != MH_OK) {
        mclog::Info("hook {}: failed", name);
        return false;
    }
    mclog::Info("hooked {}", name);
    return true;
}

// Runs fn, turning an access violation inside it into `false` (engine calls made on guesses).
template <class Fn>
bool Guarded(Fn&& fn)
{
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

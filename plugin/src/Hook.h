#pragma once

#include "Log.h"

#include <MinHook.h>

#include <mutex>

// MinHook's MH_EnableHook suspends and resumes every thread in the process: one Freeze/Unfreeze per
// call, ~44 ms each measured on this machine. Installing the ~17 hooks one at a time therefore spent
// ~0.75 s of real time at startup with the game window up and the hook table half patched, and
// docs/HANDOFF.md §8.1 keeps exactly that window as the strongest remaining suspect for the
// intermittent startup crash at maxpayne2.exe+0x1b80 (MP2's own window procedure, ecx = 0). Opening
// one batch around Startup takes seventeen suspensions down to one. Hypothesis, not a proven fix:
// it takes ~0.75 s off the startup window (~17x), and only the user's own launches can say whether
// it was the cause.

namespace hooks
{
    namespace detail
    {
        // Guards the batch flag against the callers that install a hook from inside a detour:
        // Render.cpp hooks seven vtable slots as soon as MP2 has a device, on whatever thread made
        // it, which can be while Startup is still inside its batch. Without this lock one of those
        // can be staged just after FlushHooks() has run and would sit in the hook table, disabled,
        // for the rest of the session.
        inline std::mutex& Gate()
        {
            static std::mutex gate;  // constexpr-constructed: already zeroed before any thread runs
            return gate;
        }

        // True while every InstallHook() is only creating. Reentrancy is not nesting: a batch is
        // opened by Startup and closed by the same thread before it returns.
        inline bool& Batching()
        {
            static bool batching = false;
            return batching;
        }
    }

    // Everything installed from here until FlushHooks() is created but left disabled.
    inline void BeginBatch()
    {
        std::lock_guard lock(detail::Gate());
        detail::Batching() = true;
    }

    // One Freeze/Unfreeze for the whole batch: EnableAllHooksLL suspends the threads once and then
    // patches every created hook, where EnableHook() per target suspends and resumes them again.
    // False if MinHook refused, in which case nothing in this batch is hooked - the per-hook "hooked"
    // lines above it already said the detours were built, and the startup summary says the run is
    // incomplete.
    inline bool FlushHooks()
    {
        std::lock_guard lock(detail::Gate());
        detail::Batching() = false;
        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            mclog::Info("MinHook failed to enable the hooks");
            return false;
        }
        return true;
    }
}

// Detours `target` to `detour` and stores the trampoline in `original`.
// MSVC can't put __thiscall on free functions, so detours of member functions are written as
// __fastcall(self, edx-dummy, args...): `this` arrives in ECX either way.
//
// Inside a batch (BeginBatch/FlushHooks) the hook is created and left for the flush; outside one it
// is enabled here, which is what the callers that install from inside a detour need. A missing
// target logs and returns false, so one unresolved engine symbol costs that one hook and nothing
// else.
template <class T>
bool InstallHook(void* target, void* detour, T& original, const char* name)
{
    if (!target) {
        mclog::Info("hook {}: no target", name);
        return false;
    }
    std::lock_guard lock(hooks::detail::Gate());
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&original)) != MH_OK) {
        mclog::Info("hook {}: failed", name);
        return false;
    }
    if (!hooks::detail::Batching() && MH_EnableHook(target) != MH_OK) {
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

#pragma once

namespace crashlog
{
    // Records the three exceptions that end a game - access violations, illegal instructions and
    // stack overflows - in MaxCraft.log through a vectored handler, so it sees them before anything
    // else can handle them: where, the registers, the calls on the stack and the bytes around the
    // instruction. It only logs; whoever would have handled the exception still does. Eight reports
    // per process, then it stays quiet.
    //
    // The handler must not allocate and must not throw, because an exception escaping a vectored
    // handler is fatal and takes the log with it: the report is built in a fixed buffer and written
    // with mclog::WriteRaw(), which cannot allocate either.
    void Install();
}

#pragma once

namespace crashlog
{
    // Records access violations (where, registers, and the calls on the stack) in MaxCraft.log.
    // It only logs: whoever would have handled the exception still does.
    void Install();
}

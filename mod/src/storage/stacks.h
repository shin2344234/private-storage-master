#pragma once
#include <cstdint>

// Bigger stacks of stackable items, the feature Fat Stacks (Nexus 157) gives by
// replacing game data. This does it at runtime instead: it hooks the reader that
// fills each ItemInfo record and multiplies the record's own stack limit, so a
// game patch moves the code without breaking the feature and no game file is
// touched. Research is private\research\R9-stack-size.md.
//
// StackMultiplier=1 means the game's own limits, and then nothing here runs and
// no hook is installed. Master Stack, the standalone mod built from this code,
// takes precedence: when MasterStack.asi is loaded this stands down and says so,
// so the two can never multiply the same limit twice.
namespace psm::stacks
{
    // Why stacks are not being changed. The numbers are the STACK_STANDDOWN_*
    // values in include/stack_api.h and must stay in step with them.
    enum Reason
    {
        kApplying = 0,
        kOff = 1,          // StackMultiplier is 1
        kOtherMod = 2,     // Master Stack is installed and does it instead
        kNoAnchor = 3,     // the item table reader was not found
        kHookFailed = 4,
        kTooLate = 5,      // the game read its item table before the hook went in
    };

    void Start();
    // One line saying what the item table came out as, once the game has read it.
    // Called from the startup thread after the wait that storage keys need.
    void Flush();
    void Stop();

    struct Report
    {
        bool hooked = false;
        Reason reason = kOff;
        int multiplier = 1;          // in force this launch; 1 when nothing is changed
        int patched = 0;
        int64_t biggest = 0;
        int unstackable = 0;
    };
    Report Status();
}

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

    // Raise the limits already in memory to a bigger multiplier, without a
    // restart. Only upwards: a slot can hold more than the game allows, so
    // lowering a limit under a stack that is already over it is left to the next
    // launch, where the game builds everything from the raised data itself.
    //
    // False when it cannot be done now, with the reason in `why`: no hook this
    // session, a multiplier no higher than the one in force, or anything other
    // than free play with no storage open. The caller decides what to do next;
    // the setting is saved either way by whoever called it.
    bool RaiseNow(int multiplier, char* why, size_t whyLen);
    // Whether a raise could apply this session at all, which is what the hook
    // being installed comes down to.
    bool CanRaiseNow();

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

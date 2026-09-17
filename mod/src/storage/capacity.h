#pragma once

// Storage sizes. The InventoryInfo reader is hooked and, after it returns, the
// default and maximum slot counts of the configured inventories are shifted by
// the same amount before any save builds its storage from them
// (the reader is hooked, not the data). Live storage is never written,
// so turning a setting off restores the game's sizes on the next start and
// purchased expansions survive either way.
namespace psm::capacity
{
    struct SizeInfo
    {
        bool known = false;        // the record has been read this launch
        int gameDefault = 0;       // from the game data
        int gameMax = 0;
        int appliedDefault = 0;    // what the record holds now
        int liveCapacity = -1;     // the character's storage, -1 with no save loaded
        int liveUsed = -1;         // filled slots
        int slotArray = 0;
        int extras = 0;            // expansions bought plus story slots
    };

    // Called as early as possible: sizes only apply to storage built after the hook.
    void Start();
    // Log every inventory record and every storage the player has.
    void RequestDump();
    void Stop();

    bool Hooked();
    int LearnedPrivateExtras();
    // Refreshed about once a second while something keeps asking.
    void GetSize(int storage, SizeInfo& out);

    // Deposit probe (R5), DebugLog=1 only. Request from any thread; the tick runs
    // it on the game's main thread and logs the result half a second later.
    void RequestDepositProbe();
    void DepositProbeTick();
}

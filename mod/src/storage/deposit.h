#pragma once
#include <cstdint>

// Loot straight into storage (private/design/auto-store.md).
//
// Master Looter reports a pickup with Queue(item, gained) from any thread. Tick,
// on the game's main thread, finds the item in the bag, offers it to the enabled
// storages in a fixed order through the game's own move check, sends it to the
// first that takes it, and confirms on later frames that the bag gave it up. A
// storage the server refuses is skipped for that item and the next one is tried.
// Every move is one a player could make in the warehouse screen.
namespace psm::deposit
{
    // Same numbers as PSM_DEPOSIT_* in psm_api.h.
    enum Reason : int32_t
    {
        kStored = 1,
        kNoStorageTakesIt = 2,
        kStorageFull = 3,
        kNeverMoved = 4,
        kLocked = 5,
        kNotInBag = 6,
        kOff = 7,
        kBusy = 8,
    };

    struct Result
    {
        uint16_t item;
        int16_t storage;   // PSM storage index, -1 when it stayed in the bag
        int32_t reason;
        int64_t moved;
    };

    // Any thread. False when auto-store is off, the move function is missing or
    // the queue is full.
    bool Queue(uint16_t item, int64_t gained);
    // Game thread, every frame. canMove: free play with no storage screen open.
    void Tick(bool canMove);
    // Copies out and forgets finished deposits, oldest first.
    int TakeResults(Result* out, int max);
    // The move function was found, so deposits can work at all.
    bool Available();

    // Ctrl+F11 with DebugLog=1: deposit half of the next bag stack, through the
    // same path Master Looter uses, ignoring only the AutoStore master switch.
    void DebugNextBagStack();
}

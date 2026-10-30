#pragma once
#include <cstdint>

// Game addresses, found at runtime. Functions come from byte patterns
// (written against exe 1.0.0.3392, the DLC build), classes from their RTTI names, globals
// from the instructions that read them. Nothing is a fixed offset from the image
// base, so a game patch that moves code costs nothing, and one that changes
// code the mod relies on turns the affected feature off with a log line.
namespace psm::addr
{
    struct Storage
    {
        uintptr_t eventPost = 0;       // UIEventWrap<...StageChartUIControlCommandData>::Post
        uintptr_t requestPhase = 0;    // (phase manager, phase, on)
        uintptr_t modeSwitch = 0;      // once per frame on the main thread, arg is the phase manager
        uintptr_t stageClose = 0;      // "Close" to a stage by id
        uintptr_t inputBlockSet = 0;   // (stage manager, &stage id, mode)
        uintptr_t warehouseHandler = 0;// UIGamePlayControlRootWarehouse2's packet handler, slot 146 on 3392
        uintptr_t warehouseVtable = 0;
        uintptr_t eventWrapVtable = 0;
        uintptr_t stageMgrVtable = 0;
        uintptr_t eventManagerGlobal = 0;
        uintptr_t actorManagerGlobal = 0;
        // Structure offsets that have moved in game updates before, and so are
        // read out of the code rather than written down.
        unsigned  phaseModeOff = 0;     // phase manager + this = the mode byte (4 is play)
        unsigned  phaseScreenOff = 0;   // phase manager + this = the current screen byte
        unsigned  phaseQueuedOff = 0;   // phase manager + this = the count of queued phase requests
        unsigned  eventWrapOff = 0;     // UIEventManager + this = the StageChartUIControl wrap
        // Names of what ResolveStorage could not find, comma separated, for the
        // one line that tells the player why the storage keys are off.
        char      missing[192] = {};
    };

    struct Capacity
    {
        uintptr_t inventoryInfoRead = 0;   // (stream, record) -> bool; record layout in inventory.h
        uintptr_t invMgrVtable = 0;
        uintptr_t invMgrGlobal = 0;
        uintptr_t actorManagerGlobal = 0;
        uintptr_t clientMoveItem = 0;      // R5: client check and send for an inventory-to-inventory move
        uintptr_t inventoryHolderOf = 0;   // R3C 1.2: GetInventoryHolder(actor), follows a borrowed bag
    };

    struct Stacks
    {
        uintptr_t itemInfoRead = 0;   // (stream, ItemInfo record) -> bool
    };

    // Each returns false and logs what is missing.
    bool ResolveStorage(Storage& out);
    bool ResolveCapacity(Capacity& out);
    bool ResolveStacks(Stacks& out);
}

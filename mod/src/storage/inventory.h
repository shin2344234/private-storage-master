#pragma once
#include <cstddef>
#include <cstdint>

#include "core/settings.h"
#include "game/addresses.h"

// Reads of the game's inventory tables, shared by the size code and deposits.
// All of it is plain memory reads that fail closed, except Move, which calls the
// game's own client check for an inventory-to-inventory move (R5).
namespace psm::inv
{
    // Record names by PSM storage index, the same order as Settings.
    inline constexpr const char* kRecordNames[Settings::kStorages] = {
        "CampWareHouse", "Housing_GatheredMaterials", "Housing_Dresser", "Housing_Refrigerator", "Housing_Collecting",
        "CampStraw", "BirdFeed", "WareHouse", "Kuku"};

    // Set once by capacity::Start when the addresses resolve. Until then every
    // read returns nothing.
    void SetAddresses(const addr::Capacity& a);
    bool Ready();
    bool CanMove();   // the move function was found too

    uintptr_t Manager(uint32_t* count, uintptr_t* records);
    bool RecordName(uintptr_t rec, char* out, size_t cap);
    bool IndexName(uint16_t index, char* out, size_t cap);
    uintptr_t RecordByName(const char* want);
    int IndexByName(const char* want);

    uintptr_t PlayerCharacter();
    uintptr_t PlayerHolder();          // the controlled character's inventory holder (R3C 5)
    uintptr_t BucketByIndex(uintptr_t holder, uint16_t index);

    struct Slot
    {
        uint64_t instance = 0;
        uint16_t item = 0xFFFF;
        uint16_t variant = 0;
        int64_t count = 0;
        uint8_t locked = 0;
    };
    uint32_t SlotCount(uintptr_t bucket);
    bool SlotAt(uintptr_t bucket, uint32_t k, Slot& out);
    // Every slot holding this item, summed; -1 when the bucket cannot be read.
    int64_t ItemTotal(uintptr_t bucket, uint16_t item);

    // Index of the plain (type 0) entry from -> to in a record's move list (R3C 1.1), or -1.
    int MoveIndex(uintptr_t rec, uint16_t from, uint16_t to);

    // The game's error hashes the move can write.
    enum : uint32_t
    {
        kErrNone = 0,
        kErrNotWritten = 0xFFFFFFFF,
        kErrCantMoveItem = 0xED0EF13C,
        kErrSlotNotExist = 0xD2023F88,
        kErrNoMoreInsert = 0x92ADE5AA,
    };
    const char* ErrName(uint32_t e);

    // Client check and send for one move from the player's own inventory to
    // another. False only when the call raised an exception; *err holds the game's
    // answer, 0 when the request was sent.
    bool Move(uintptr_t holder, uint32_t* err, uint32_t actor, uint16_t item, uint16_t variant, uint64_t count, uint16_t fromInventory,
              uint16_t slot, uint32_t moveIndex);
}

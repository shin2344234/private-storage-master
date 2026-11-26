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
    // Record i of the table Manager returned. 1.0.0.3663 changed the entry layout.
    bool RecordAt(uintptr_t records, uint32_t i, uintptr_t* rec);
    // The name of record i, whether or not the record itself is loaded. The game
    // reads each record from the data file on first use and drops it again after
    // 30 s unused, so RecordAt can say no for a record whose name is known.
    bool EntryName(uintptr_t records, uint32_t i, char* out, size_t cap);
    // The name of a record the game has in hand. From 1.0.0.3840 the record has no
    // name of its own, so this finds its table entry by the record's key.
    bool RecordName(uintptr_t rec, char* out, size_t cap);
    bool IndexName(uint16_t index, char* out, size_t cap);
    uintptr_t RecordByName(const char* want);
    int IndexByName(const char* want);

    // InventoryInfo record fields on exe 1.0.0.3392, which reordered the record
    // small fields first (2.03 had the name at +0x08, default +0x48, max +0x4A,
    // needSave +0x9D). ResolveCapacity reads where the small fields are out of the
    // reader, since 1.0.0.3840 moved them again and dropped the name from the record:
    //                 3392 to 3732   3840
    //   name          +0x00 (node)   in the table entry, see Capacity::invMgrEntryName
    //   key           +0x08 (u16)    +0x00 (u16)
    //   needSave      +0x0D          +0x0B       one byte: the save keeps the slot count
    //   default       +0x10          +0x0E       two bytes
    //   max           +0x14          +0x12       two bytes
    constexpr unsigned kRecName = 0x00;       // staticstringA node, first qword the text (3392 to 3732)
    constexpr unsigned kRecKey = 0x00;        // u16 on 3840, the same value as the entry's key
    unsigned RecNeedSave();
    unsigned RecDefault();
    unsigned RecMax();
    constexpr unsigned kRecMoveList = 0x38;   // pointer, count at +0x40, element stride 0xA0
    // _collectionItemList: pointer, u32 count at +0x98, u32 capacity at +0x9C,
    // 16-byte elements {u16 item, s64 limit at +8}. Read on 3663 from the field's
    // reader (+179D970), which the deserializer (+177CD80) calls for the field
    // its error string names. Housing_Collecting has 997 entries, each limit 1.
    constexpr unsigned kRecCollection = 0x90;
    constexpr unsigned kRecCollectionCount = 0x98;

    uintptr_t PlayerCharacter();
    // The bag of whoever is being played, as the game itself resolves it, borrowed
    // bag included (Damiane and Oongka use Kliff's). Game thread only, since it
    // calls the game's GetInventoryHolder.
    uintptr_t PlayerBag();
    // Game thread, every frame: publishes PlayerBag for the other threads.
    void RefreshPlayerBag();
    // Any thread: the bag the last frame published, or plain reads when no frame
    // has run in the last second (R3C 5).
    uintptr_t PlayerHolder();
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

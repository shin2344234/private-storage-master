#include "storage/inventory.h"

#include <Windows.h>
#include <atomic>
#include <cstring>

#include "game/mem.h"

namespace psm::inv
{
    namespace
    {
        addr::Capacity g_addr;
        std::atomic<bool> g_ready{false};

        using MoveFn = void(__fastcall*)(uintptr_t holder, uint32_t* err, uint32_t actorA, uint32_t actorB, uint16_t item,
                                         uint16_t variant, uint64_t count, uint16_t fromInventory, uint16_t srcSlot, uint32_t moveIndex);
    }

    void SetAddresses(const addr::Capacity& a)
    {
        g_addr = a;
        g_ready.store(true, std::memory_order_release);
    }

    bool Ready() { return g_ready.load(std::memory_order_acquire); }
    // The move and every read a deposit needs to find the bag and the storages.
    bool CanMove() { return Ready() && g_addr.clientMoveItem && g_addr.invMgrGlobal && g_addr.invMgrVtable && g_addr.actorManagerGlobal; }

    uintptr_t Manager(uint32_t* count, uintptr_t* records)
    {
        uintptr_t mgr = 0;
        if (!Ready() || !g_addr.invMgrGlobal || !mem::ReadPtr(g_addr.invMgrGlobal, &mgr)) return 0;
        uintptr_t vt = 0;
        if (!mem::ReadPtr(mgr, &vt) || vt != g_addr.invMgrVtable) return 0;
        if (!mem::Read32(mgr + 0x08, count) || *count == 0 || *count > 256 || !mem::ReadPtr(mgr + 0x58, records)) return 0;
        return mgr;
    }

    bool RecordName(uintptr_t rec, char* out, size_t cap)
    {
        uintptr_t node = 0, text = 0;
        out[0] = 0;
        return mem::ReadPtr(rec + 8, &node) && mem::ReadPtr(node, &text) && mem::ReadCString(text, out, cap);
    }

    bool IndexName(uint16_t index, char* out, size_t cap)
    {
        uint32_t n = 0;
        uintptr_t arr = 0, rec = 0;
        out[0] = 0;
        return Manager(&n, &arr) && index < n && mem::ReadPtr(arr + 8ull * index, &rec) && RecordName(rec, out, cap);
    }

    uintptr_t RecordByName(const char* want)
    {
        uint32_t n = 0;
        uintptr_t arr = 0;
        if (!Manager(&n, &arr)) return 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            uintptr_t rec = 0;
            char name[40];
            if (mem::ReadPtr(arr + 8ull * i, &rec) && RecordName(rec, name, sizeof name) && strcmp(name, want) == 0) return rec;
        }
        return 0;
    }

    int IndexByName(const char* want)
    {
        uint32_t n = 0;
        uintptr_t arr = 0;
        if (!Manager(&n, &arr)) return -1;
        for (uint32_t i = 0; i < n; ++i)
        {
            uintptr_t rec = 0;
            char name[40];
            if (mem::ReadPtr(arr + 8ull * i, &rec) && RecordName(rec, name, sizeof name) && strcmp(name, want) == 0) return static_cast<int>(i);
        }
        return -1;
    }

    uintptr_t PlayerCharacter()
    {
        uintptr_t g = 0, mgr = 0, user = 0, ch = 0;
        if (!Ready() || !g_addr.actorManagerGlobal || !mem::ReadPtr(g_addr.actorManagerGlobal, &g) || !mem::ReadPtr(g + 0x30, &mgr) ||
            !mem::ReadPtr(mgr + 0x58, &user) || !mem::ReadPtr(user + 0xD8, &ch))
            return 0;
        return ch;
    }

    namespace
    {
        // actor->[+0x68]->[+0xB8], accepted only when the holder names the actor as
        // its owner.
        uintptr_t OwnHolder(uintptr_t actor)
        {
            uintptr_t comp = 0, holder = 0, owner = 0;
            if (!actor || !mem::ReadPtr(actor + 0x68, &comp) || !mem::ReadPtr(comp + 0xB8, &holder)) return 0;
            if (!mem::ReadPtr(holder + 8, &owner) || owner != actor) return 0;
            return holder;
        }

        using HolderOfFn = uintptr_t(__fastcall*)(uintptr_t actor);

        // Plain reads, for when the game's lookup is missing or has not run yet.
        // The character's own holder first, then the one it borrows (R3C 1.2).
        // Damiane has a holder of her own that the game never uses (0 of 50 on
        // 19 September), so this can pick the wrong bag; the game's answer below
        // replaces it once a frame has run.
        uintptr_t ReadHolder()
        {
            const uintptr_t ch = PlayerCharacter();
            if (const uintptr_t own = OwnHolder(ch)) return own;
            uintptr_t link = 0, lender = 0;
            if (!ch || !mem::ReadPtr(ch + 0xA0, &link) || !mem::ReadPtr(link + 0xD0, &lender) || lender == ch) return 0;
            return OwnHolder(lender);
        }

        // The game's answer, published by the frame tick for the other threads.
        std::atomic<uintptr_t> g_bag{0};
        std::atomic<ULONGLONG> g_bagAt{0};
        constexpr ULONGLONG kBagFreshMs = 1000;
    }

    uintptr_t PlayerBag()
    {
        const uintptr_t ch = PlayerCharacter();
        if (!ch || !g_addr.inventoryHolderOf) return ReadHolder();
        uintptr_t holder = 0;
        __try { holder = reinterpret_cast<HolderOfFn>(g_addr.inventoryHolderOf)(ch); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return ReadHolder(); }
        uintptr_t owner = 0;
        // The owner of a real holder is an actor; take nothing the game did not.
        return holder && mem::ReadPtr(holder + 8, &owner) && owner ? holder : 0;
    }

    void RefreshPlayerBag()
    {
        if (!Ready() || !g_addr.inventoryHolderOf) return;
        g_bag.store(PlayerBag(), std::memory_order_relaxed);
        g_bagAt.store(GetTickCount64(), std::memory_order_release);
    }

    uintptr_t PlayerHolder()
    {
        const ULONGLONG at = g_bagAt.load(std::memory_order_acquire);
        if (at && GetTickCount64() - at <= kBagFreshMs) return g_bag.load(std::memory_order_relaxed);
        return ReadHolder();
    }

    uintptr_t BucketByIndex(uintptr_t holder, uint16_t index)
    {
        uintptr_t arr = 0;
        uint32_t n = 0;
        if (!holder || !mem::ReadPtr(holder + 0x18, &arr) || !mem::Read32(holder + 0x20, &n) || n > 128) return 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            uintptr_t bk = 0;
            uint16_t idx = 0xFFFF;
            if (mem::ReadPtr(arr + 8ull * i, &bk) && mem::Read16(bk + 0x10, &idx) && idx == index) return bk;
        }
        return 0;
    }

    uint32_t SlotCount(uintptr_t bucket)
    {
        uint32_t size = 0;
        if (!bucket || !mem::Read32(bucket + 0x08, &size)) return 0;
        size &= 0xFFFF;
        return size <= 4096 ? size : 0;
    }

    bool SlotAt(uintptr_t bucket, uint32_t k, Slot& out)
    {
        uintptr_t slots = 0;
        out = Slot{};
        const uintptr_t s = bucket && mem::ReadPtr(bucket, &slots) ? slots + 0xC8ull * k : 0;
        return s && mem::Read16(s + 0x08, &out.item) && mem::Read16(s + 0x0A, &out.variant) &&
               mem::ReadBytes(s + 0x10, &out.count, sizeof out.count) && mem::Read8(s + 0xA1, &out.locked) && mem::Read64(s, &out.instance);
    }

    int64_t ItemTotal(uintptr_t bucket, uint16_t item)
    {
        const uint32_t size = SlotCount(bucket);
        uintptr_t slots = 0;
        if (!size || !mem::ReadPtr(bucket, &slots)) return -1;
        int64_t total = 0;
        for (uint32_t k = 0; k < size; ++k)
        {
            // Only the two fields that matter: a deposit reads this for every storage it offers to.
            uint16_t id = 0xFFFF;
            int64_t count = 0;
            const uintptr_t s = slots + 0xC8ull * k;
            if (mem::Read16(s + 0x08, &id) && id == item && mem::ReadBytes(s + 0x10, &count, sizeof count) && count > 0) total += count;
        }
        return total;
    }

    int MoveIndex(uintptr_t rec, uint16_t from, uint16_t to)
    {
        uintptr_t list = 0;
        uint32_t n = 0;
        if (!mem::ReadPtr(rec + 0x38, &list) || !mem::Read32(rec + 0x40, &n) || n > 64) return -1;
        for (uint32_t i = 0; i < n; ++i)
        {
            const uintptr_t e = list + 0xA0ull * i;
            uint8_t type = 3;
            uint16_t f = 0xFFFF, t = 0xFFFF;
            if (mem::Read8(e, &type) && mem::Read16(e + 2, &f) && mem::Read16(e + 4, &t) && type == 0 && f == from && t == to)
                return static_cast<int>(i);
        }
        return -1;
    }

    const char* ErrName(uint32_t e)
    {
        switch (e)
        {
        case 0:          return "sent";
        case 0xFFFFFFFF: return "error not written";
        case 0x73353994: return "eErrNoInvalidInventory";
        case 0x982103B7: return "eErrNoInvalidTrData";
        case 0x3B331F28: return "eErrNoInvalidActorKey";
        case 0x3E2AD36C: return "eErrNoInvalidInteractionDistance";
        case 0xED0EF13C: return "eErrNoCantMoveItem";
        case 0xA187B201: return "eErrNoInvalidInventorySlotNoNotAssert";
        case 0xD2023F88: return "eErrNoInventorySlotNotExist";
        case 0x1E807FD1: return "eErrNoAlreadyExistItem";
        case 0x92ADE5AA: return "eErrNoNoMoreInsertItem";
        case 0xC36792C3: return "eErrNoInvalidItemCount";
        case 0xF45703E9: return "eErrNoCannotPopHideOnlyQuestItem";
        case 0x171B4DE6: return "eErrNoSocketLocked";
        case 0xD65F8D70: return "eErrNoActorNotExist";
        case 0x6C57E0EE: return "eErrNoCannotFindExchangeItemAsPrice";
        case 0x6308EF12: return "eErrNoInvalidSlotNo";
        case 0xEFD37ACC: return "eErrNoDontUseStoreCondition";
        case 0xA13ED06F: return "eErrNoMoneyIsLack";
        default:         return "unnamed";
        }
    }

    bool Move(uintptr_t holder, uint32_t* err, uint32_t actor, uint16_t item, uint16_t variant, uint64_t count, uint16_t fromInventory,
              uint16_t slot, uint32_t moveIndex)
    {
        if (!CanMove()) return false;
        __try
        {
            reinterpret_cast<MoveFn>(g_addr.clientMoveItem)(holder, err, actor, actor, item, variant, count, fromInventory, slot, moveIndex);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
}

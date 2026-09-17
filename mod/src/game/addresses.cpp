#include "game/addresses.h"

#include <Windows.h>
#include <cstring>

#include "core/log.h"
#include "game/mem.h"

namespace psm::addr
{
    namespace
    {
        unsigned long long R(uintptr_t a) { return static_cast<unsigned long long>(a ? a - mem::Game().base : 0); }

        uintptr_t Unique(const char* what, const char* pattern)
        {
            size_t hits = 0;
            const uintptr_t a = mem::FindUnique(pattern, &hits);
            if (a) LOG("[addr] %s at +%llX", what, R(a));
            else LOG_ERR("[addr] %s: pattern matched %zu places, expected one", what, hits);
            return a;
        }

        // A global read by the rip-relative instruction at the start of the pattern.
        uintptr_t Global(const char* what, const char* pattern, unsigned instrLen)
        {
            const uintptr_t site = Unique(what, pattern);
            if (!site) return 0;
            const uintptr_t g = mem::RipAt(site, instrLen);
            if (!mem::InImage(g)) { LOG_ERR("[addr] %s: the instruction at +%llX does not point into the game", what, R(site)); return 0; }
            return g;
        }

        uintptr_t Vtable(const char* what, const char* decorated)
        {
            uintptr_t vt[4] = {};
            const int n = mem::FindVtablesByName(decorated, vt, 4);
            if (n == 1) { LOG("[addr] %s vtable +%llX", what, R(vt[0])); return vt[0]; }
            LOG_ERR("[addr] %s: %d vtables named %s, expected one", what, n, decorated);
            return 0;
        }

        // Primary function entry for an address inside it, following chained unwind info.
        uintptr_t FunctionEntry(uintptr_t inside)
        {
            DWORD64 imageBase = 0;
            for (int hop = 0; hop < 8; ++hop)
            {
                const RUNTIME_FUNCTION* rf = RtlLookupFunctionEntry(inside, &imageBase, nullptr);
                if (!rf) return 0;
                const uintptr_t begin = static_cast<uintptr_t>(imageBase + rf->BeginAddress);
                uint8_t verFlags = 0, count = 0;
                const uintptr_t unwind = static_cast<uintptr_t>(imageBase + (rf->UnwindData & ~1u));
                if (!mem::Read8(unwind, &verFlags) || !mem::Read8(unwind + 2, &count)) return 0;
                if (!((verFlags >> 3) & 4)) return begin;   // not UNW_FLAG_CHAININFO
                uint32_t parent = 0;
                if (!mem::Read32(unwind + 4 + ((count + 1) & ~1u) * 2, &parent)) return 0;
                inside = static_cast<uintptr_t>(imageBase + parent);
            }
            return 0;
        }

        bool FindWithin(uintptr_t from, size_t len, const char* pattern)
        {
            for (size_t i = 0; i < len; ++i)
                if (mem::MatchAt(from + i, pattern)) return true;
            return false;
        }
    }

    bool ResolveStorage(Storage& s)
    {
        s.eventPost = Unique("EventPost", "4C 8B DC 49 89 5B 18 49 89 73 20 89 54 24 10 57 41 56 41 57 48 81 EC 90 00 00 00 4D 8B F1 4D 8B F8 8B FA 48 8B F1 80 79 1A 00");
        s.requestPhase = Unique("RequestPhase", "48 89 5C 24 10 48 89 6C 24 20 56 57 41 56 48 83 EC 30 41 0F B6 F0 0F B6 EA 4C 8B F1 41 B0 01 48 8B 11 48 8D 4C 24 20 E8 ?? ?? ?? ??");
        s.modeSwitch = Unique("ModeSwitch", "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 48 8D AC 24 F0 FE FF FF 48 81 EC 10 02 00 00 48 8B D9");
        s.stageClose = Unique("StageClose", "48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 4D 8B E1 49 8B F0 48 8B DA");
        s.inputBlockSet = Unique("InputBlockSet", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 44 88 44 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 40 41 0F B6 E8 4C 8B FA");
        s.eventManagerGlobal = Global("EventManager", "48 8B 1D ?? ?? ?? ?? 48 89 5C 24 78 48 8B 03 48 8B CB FF 50 08 90 0F B7 06 66 89 44 24 60", 7);
        s.actorManagerGlobal = Global("ActorManager", "48 8B 0D ?? ?? ?? ?? 48 8B 49 58 E8 ?? ?? ?? ?? 90 40 38 74 24 40 40 0F 94 C5 48 8D 05 ?? ?? ?? ??", 7);
        s.warehouseVtable = Vtable("Warehouse2", ".?AVUIGamePlayControlRootWarehouse2@uiCommonScript@pa@@");
        s.eventWrapVtable = Vtable("StageChartUIControl event wrap",
            ".?AV?$UIEventWrap@$$CBVActorKey@pa@@AEBVstaticstringA@2@AEBV32@AEBVSequencerStageId@2@AEBVStageChartUIControlCommandData@2@XXXX@pa@@");
        s.stageMgrVtable = Vtable("ClientSequencerStageManager", ".?AVClientSequencerStageManager@pa@@");
        if (s.warehouseVtable)
        {
            uintptr_t slot = 0;
            if (mem::ReadPtr(s.warehouseVtable + 8 * 144, &slot) && mem::InImage(slot) && mem::Executable(slot, 16))
            {
                s.warehouseHandler = slot;
                LOG("[addr] warehouse command handler (slot 144) +%llX", R(slot));
            }
            else LOG_ERR("[addr] Warehouse2 slot 144 is not code");
        }
        return s.eventPost && s.requestPhase && s.modeSwitch && s.stageClose && s.inputBlockSet && s.eventManagerGlobal &&
               s.actorManagerGlobal && s.warehouseVtable && s.eventWrapVtable && s.stageMgrVtable && s.warehouseHandler;
    }

    bool ResolveCapacity(Capacity& c)
    {
        // The reads of _defaultSlotCount (+0x48) and _maxSlotCount (+0x4A); the
        // record is the second argument, kept in rdi.
        const uintptr_t fields = Unique("InventoryInfo field reads",
            "48 8B 03 48 8D 57 48 41 B8 02 00 00 00 48 8B CB FF 50 08 84 C0 75 ?? 48 8D 05 ?? ?? ?? ?? E9 ?? ?? ?? ?? "
            "48 8B 03 48 8D 57 4A 41 B8 02 00 00 00 48 8B CB FF 50 08 84 C0");
        if (fields)
        {
            const uintptr_t entry = FunctionEntry(fields);
            if (!entry || fields - entry > 0x400)
                LOG_ERR("[addr] InventoryInfo reader: no function entry for +%llX", R(fields));
            else if (!mem::MatchAt(entry, "48 89 5C 24 ?? 57 48 83 EC ?? 48 8B 01 48 8B FA"))
                LOG_ERR("[addr] InventoryInfo reader at +%llX does not start the way it did on 2.02", R(entry));
            else if (!FindWithin(entry, 0x80, "48 8D 57 08 48 8B CB") || !FindWithin(entry, 0x400, "48 8D 97 9D 00 00 00 41 B8 01 00 00 00"))
                LOG_ERR("[addr] InventoryInfo reader at +%llX no longer reads _stringKey at +0x08 and _needSaveSlotCount at +0x9D", R(entry));
            else
            {
                c.inventoryInfoRead = entry;
                LOG("[addr] InventoryInfo reader +%llX", R(entry));
            }
        }

        c.invMgrVtable = Vtable("InventoryInfoManager", ".?AVInventoryInfoManager@pa@@");
        if (c.invMgrVtable)
        {
            uintptr_t set = 0, clear = 0;
            mem::ReadPtr(c.invMgrVtable + 8 * 2, &set);
            mem::ReadPtr(c.invMgrVtable + 8 * 3, &clear);
            // slot 2: mov [rip+g], rcx; ret    slot 3: mov qword [rip+g], 0; ret
            if (set && clear && mem::MatchAt(set, "48 89 0D ?? ?? ?? ?? C3") && mem::MatchAt(clear, "48 C7 05 ?? ?? ?? ?? 00 00 00 00 C3"))
            {
                const uintptr_t a = mem::RipAt(set, 7);
                uint32_t d = 0;
                mem::Read32(clear + 3, &d);
                const uintptr_t b = clear + 11 + static_cast<int32_t>(d);
                if (a == b && mem::InImage(a)) c.invMgrGlobal = a;
            }
            if (c.invMgrGlobal) LOG("[addr] InventoryInfoManager global +%llX", R(c.invMgrGlobal));
            else LOG_ERR("[addr] InventoryInfoManager slots 2 and 3 do not name one global");
        }
        c.actorManagerGlobal = Global("ActorManager", "48 8B 0D ?? ?? ?? ?? 48 8B 49 58 E8 ?? ?? ?? ?? 90 40 38 74 24 40 40 0F 94 C5 48 8D 05 ?? ?? ?? ??", 7);
        return c.inventoryInfoRead && c.invMgrGlobal;
    }
}

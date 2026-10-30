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

        // The one place in [from, from + len) the pattern matches, or 0 when it
        // matches nowhere or more than once.
        uintptr_t UniqueWithin(uintptr_t from, size_t len, const char* pattern, int* hitsOut)
        {
            uintptr_t hit = 0;
            int hits = 0;
            for (size_t i = 0; i < len; ++i)
                if (mem::MatchAt(from + i, pattern) && !hits++) hit = from + i;
            *hitsOut = hits;
            return hits == 1 ? hit : 0;
        }

        // The Warehouse2 packet handler opens with mov rbx, r9 (the packet) and
        // then returns unless the packet's first byte is 0x15. It was slot 144 up
        // to 2.03.02 and is slot 146 on the DLC build, which put two slots ahead
        // of it and left a shared `ret 0` stub in slot 144. Exactly one slot of
        // the 173 has that shape, so look for it rather than at a number.
        uintptr_t WarehouseHandler(uintptr_t vt)
        {
            uintptr_t found = 0;
            int matches = 0, slots = 0, which = -1;
            for (; slots < 512; ++slots)
            {
                uintptr_t fn = 0;
                if (!mem::ReadPtr(vt + 8ull * slots, &fn) || !mem::InImage(fn) || !mem::Executable(fn, 16)) break;
                for (size_t i = 0; i + 6 <= 0x60; ++i)
                {
                    if (!mem::MatchAt(fn + i, "49 8B D9")) continue;
                    if (FindWithin(fn + i + 3, 0x60 - i - 3, "80 3B 15") && !matches++) { found = fn; which = slots; }
                    break;
                }
            }
            if (matches != 1)
            {
                LOG_ERR("[addr] Warehouse2: %d of its %d slots look like the packet handler, expected one", matches, slots);
                return 0;
            }
            LOG("[addr] warehouse command handler (slot %d of %d) +%llX", which, slots, R(found));
            return found;
        }

        // Every UIEventManager wrap is registered by the same four instructions,
        // so the pattern below matches once per wrap, 346 times on 2.02, 347
        // on 2.03 and 374 on the DLC build. The name passed a few instructions later is what picks ours
        // out of them, and like every other anchor here it has to pick exactly
        // one. A build that registered the name twice would otherwise hand back
        // whichever came first.
        //
        //   lea  rcx, [rsi + slot]        <- the offset wanted, at +3
        //   call ...
        //   mov  [rax+0x10], r15
        //   mov  r8d, 1
        //   lea  rdx, [rip + name]        <- at +22, 7 bytes long
        const char* const kWrapRegister =
            "48 8D 8E ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 89 78 10 41 B8 01 00 00 00 48 8D 15 ?? ?? ?? ??";

        struct WrapSearch { int named; uintptr_t hit; };

        bool CountStageChartWraps(uintptr_t hit, void* ctx)
        {
            char name[64];
            if (!mem::ReadCString(mem::RipAt(hit + 22, 7), name, sizeof name)) return false;
            if (strcmp(name, "StageChartUIControl") != 0) return false;
            auto* w = static_cast<WrapSearch*>(ctx);
            if (!w->named++) w->hit = hit;
            return w->named >= 2;   // a second one already fails the search, so stop scanning
        }
    }

    bool ResolveStorage(Storage& s)
    {
        s.eventPost = Unique("EventPost", "4C 8B DC 49 89 5B 18 49 89 73 20 89 54 24 10 57 41 56 41 57 48 81 EC 90 00 00 00 4D 8B F1 4D 8B F8 8B FA 48 8B F1 80 79 1A 00");
        s.requestPhase = Unique("RequestPhase", "48 89 5C 24 10 48 89 6C 24 20 56 57 41 56 48 83 EC 30 41 0F B6 F0 0F B6 EA 4C 8B F1 41 B0 01 48 8B 11 48 8D 4C 24 20 E8 ?? ?? ?? ??");
        // Anchored on the frame setup and the first field reads rather than the
        // prologue. The prologue is what changed between game builds: 1.0.0.2850
        // saved rbx, rsi and rdi into shadow space, 1.0.0.2944 saves only rbx and
        // pushes the other two, so a pattern starting at the entry matches one
        // build and not the other. Everything from the `lea rbp` onwards is the
        // same on both, so match there and walk back through the unwind info.
        const uintptr_t modeBody = Unique("ModeSwitch",
            "48 8D AC 24 F0 FE FF FF 48 81 EC 10 02 00 00 48 8B D9 48 8B 41 08 48 8B 50 78 4C 8B ?? 4D 85");
        if (modeBody)
        {
            const uintptr_t entry = FunctionEntry(modeBody);
            if (!entry || modeBody - entry > 0x40)
                LOG_ERR("[addr] ModeSwitch: the body at +%llX has no function entry near it", R(modeBody));
            else
            {
                s.modeSwitch = entry;
                LOG("[addr] ModeSwitch entry +%llX", R(entry));
            }
        }
        s.stageClose = Unique("StageClose", "48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 4D 8B E1 49 8B F0 48 8B DA");
        s.inputBlockSet = Unique("InputBlockSet", "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 20 44 88 44 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 40 41 0F B6 E8 4C 8B FA");
        s.eventManagerGlobal = Global("EventManager", "48 8B 1D ?? ?? ?? ?? 48 89 5C 24 78 48 8B 03 48 8B CB FF 50 08 90 0F B7 06 66 89 44 24 60", 7);
        s.actorManagerGlobal = Global("ActorManager", "48 8B 0D ?? ?? ?? ?? 48 8B 49 58 E8 ?? ?? ?? ?? 90 40 38 74 24 40 40 0F 94 C5 48 8D 05 ?? ?? ?? ??", 7);
        s.warehouseVtable = Vtable("Warehouse2", ".?AVUIGamePlayControlRootWarehouse2@uiCommonScript@pa@@");
        s.eventWrapVtable = Vtable("StageChartUIControl event wrap",
            ".?AV?$UIEventWrap@$$CBVActorKey@pa@@AEBVstaticstringA@2@AEBV32@AEBVSequencerStageId@2@AEBVStageChartUIControlCommandData@2@XXXX@pa@@");
        s.stageMgrVtable = Vtable("ClientSequencerStageManager", ".?AVClientSequencerStageManager@pa@@");
        if (s.warehouseVtable) s.warehouseHandler = WarehouseHandler(s.warehouseVtable);

        // The phase manager's mode and current-screen bytes. The screen was +0x29
        // on 2850, +0x2A on 2944 and 2976, and +0x32 on the DLC build, where the
        // mode went from +0x28 to +0x30 with it. ModeSwitch reads the two
        // together to build its mode tag, so both offsets come out of that pair.
        // The same two instructions turn up elsewhere with other registers'
        // offsets (five times on 3392), and the one inside ModeSwitch is the one.
        //   movzx r8d, byte [rbx + screen]   <- at +4
        //   movzx edx, byte [rbx + mode]     <- at +8
        if (s.modeSwitch)
        {
            int hits = 0;
            const uintptr_t pair = UniqueWithin(s.modeSwitch, 0x1000, "44 0F B6 43 ?? 0F B6 53 ??", &hits);
            uint8_t screen = 0, mode = 0;
            if (!pair)
                LOG_ERR("[addr] screen byte: ModeSwitch reads a mode and screen pair in %d places, expected one", hits);
            else if (!mem::Read8(pair + 4, &screen) || !mem::Read8(pair + 8, &mode) || mode < 0x20 || screen <= mode || screen - mode >= 8 ||
                     screen >= 0x40)
                LOG_ERR("[addr] screen byte: +%llX gives an implausible pair of offsets", R(pair));
            else
            {
                s.phaseScreenOff = screen;
                s.phaseModeOff = mode;
                LOG("[addr] phase manager mode byte at +0x%02X, screen byte at +0x%02X", mode, screen);
            }
        }

        // The phase manager's queue of phase requests, an array and its count
        // eight bytes on. ModeSwitch walks it a few instructions in:
        //   mov rdx, [rcx + array]           <- one byte at +3
        //   mov eax, [rcx + count]           <- four bytes at +6
        // +0x68 and +0x70 on 2976, +0x78 and +0x80 on the DLC build, where the
        // count no longer fits in a one-byte displacement.
        if (modeBody)
        {
            int hits = 0;
            const uintptr_t walk = UniqueWithin(modeBody, 0x60, "48 8B 51 ?? 8B 81 ?? ?? 00 00", &hits);
            uint8_t arr = 0;
            uint32_t count = 0;
            if (!walk)
                LOG_ERR("[addr] phase queue: ModeSwitch reads the queue in %d places near its start, expected one", hits);
            else if (!mem::Read8(walk + 3, &arr) || !mem::Read32(walk + 6, &count) || count != arr + 8u || count >= 0x100)
                LOG_ERR("[addr] phase queue: +%llX gives an implausible array and count", R(walk));
            else
            {
                s.phaseQueuedOff = count;
                LOG("[addr] phase manager queue count at +0x%02X", count);
            }
        }

        // The StageChartUIControl wrap's slot in UIEventManager, 0x9A8 on 2850
        // and 0x9B0 on 2944 because 2944 inserted one wrap ahead of it and
        // pushed every later slot up by eight.
        WrapSearch wrap{};
        mem::FindIf(kWrapRegister, CountStageChartWraps, &wrap);
        uint32_t slot = 0;
        if (wrap.named != 1)
            LOG_ERR("[addr] %s UIEventManager slot registers a wrap named StageChartUIControl, expected one",
                    wrap.named ? "more than one" : "no");
        else if (!mem::Read32(wrap.hit + 3, &slot) || slot < 0x100 || slot > 0x4000 || (slot & 7))
            LOG_ERR("[addr] the StageChartUIControl wrap registration at +%llX gives an implausible slot", R(wrap.hit));
        else
        {
            s.eventWrapOff = slot;
            LOG("[addr] StageChartUIControl wrap at UIEventManager+0x%X", slot);
        }

        const auto miss = [&s](uintptr_t found, const char* name)
        {
            if (found) return;
            const size_t n = strlen(s.missing);
            snprintf(s.missing + n, sizeof s.missing - n, "%s%s", n ? ", " : "", name);
        };
        miss(s.eventPost, "EventPost");
        miss(s.requestPhase, "RequestPhase");
        miss(s.modeSwitch, "ModeSwitch");
        miss(s.stageClose, "StageClose");
        miss(s.inputBlockSet, "InputBlockSet");
        miss(s.eventManagerGlobal, "EventManager");
        miss(s.actorManagerGlobal, "ActorManager");
        miss(s.warehouseVtable, "Warehouse2");
        miss(s.warehouseHandler, "warehouse handler");
        miss(s.eventWrapVtable, "event wrap");
        miss(s.stageMgrVtable, "stage manager");
        miss(s.phaseScreenOff, "screen byte");
        miss(s.phaseQueuedOff, "phase queue");
        miss(s.eventWrapOff, "wrap slot");

        return !s.missing[0];
    }

    bool ResolveCapacity(Capacity& c)
    {
        // The reads of _defaultSlotCount (+0x10) and _maxSlotCount (+0x14), with
        // the u16 at +0x12 between them; the record is the second argument, kept
        // in rdi, and the stream is called through its slot at +0x28. The DLC
        // build reordered the record small fields first, so none of the 2.03
        // offsets hold, and a build that moves them again gets no match.
        const uintptr_t fields = Unique("InventoryInfo field reads",
            "48 8D 57 10 48 8B 03 41 B8 02 00 00 00 48 8B CB FF 50 28 84 C0 75 ?? 48 8D 05 ?? ?? ?? ?? E9 ?? ?? ?? ?? "
            "48 8B 03 41 B8 04 00 00 00 48 8D 94 24 ?? ?? ?? ?? 48 8B CB FF 50 28 84 C0 0F 84 ?? ?? ?? ?? "
            "8B 94 24 ?? ?? ?? ?? 48 8D 8C 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F B7 84 24 ?? ?? ?? ?? 66 89 47 12 "
            "48 8D 57 14 48 8B 03 41 B8 02 00 00 00 48 8B CB FF 50 28 84 C0");
        if (fields)
        {
            const uintptr_t entry = FunctionEntry(fields);
            if (!entry || fields - entry > 0x400)
                LOG_ERR("[addr] InventoryInfo reader: no function entry for +%llX", R(fields));
            else if (!mem::MatchAt(entry, "48 8B C4 48 89 58 08 55 56 57 48 83 EC 60 48 8B FA 48 8B D9"))
                LOG_ERR("[addr] InventoryInfo reader at +%llX does not start the way it did on 1.0.0.3392", R(entry));
            else if (!FindWithin(entry, 0x80, "48 8B D7 48 8B CB E8") || !FindWithin(entry, 0x400, "48 8D 57 0D 48 8B 03 41 B8 01 00 00 00"))
                LOG_ERR("[addr] InventoryInfo reader at +%llX no longer reads _stringKey at +0x00 and _needSaveSlotCount at +0x0D", R(entry));
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

        // Loot storing (R5). The entry, then the fourth argument read back from its
        // home slot for the second actor lookup. The DLC build saves the four
        // register arguments relative to rsp and builds a smaller frame, so the
        // home slot is rbp+0x948 now; the ten arguments are read the same.
        const uintptr_t move = Unique("ClientMoveItem",
            "44 89 4C 24 20 44 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 "
            "48 8D AC 24 18 F7 FF FF 48 81 EC E8 09 00 00 4C 8B E2");
        if (move && FindWithin(move, 0x100, "48 8B 49 30 E8 ?? ?? ?? ?? 90 44 8B 85 48 09 00 00")) c.clientMoveItem = move;
        else if (move) LOG_ERR("[addr] ClientMoveItem at +%llX no longer looks up both actors the way it did on 1.0.0.3392", R(move));

        // The game's own holder lookup. A character whose info says mode 1 keeps
        // no bag of its own and uses another actor's, [[actor+0xA0]+0xD0], which is
        // how Damiane and Oongka reach the shared bag. The character info's mode
        // byte has moved in three updates (+0x5E, +0x5F, +0x21 on the DLC build),
        // which is why the game decides rather than the mod. On the DLC build the
        // info lookup takes its key by value in edx.
        c.inventoryHolderOf = Unique("GetInventoryHolder", "40 53 48 83 EC 20 48 8B 41 68 48 8B D9 48 8B 50 20 0F B7 52 30 E8");
        return c.inventoryInfoRead && c.invMgrGlobal;
    }

    bool ResolveStacks(Stacks& s)
    {
        // The read of ItemInfo's _maxStackCount, eight bytes into record+0x10.
        // The failure branch decides that it is this field: every field read is
        // followed by a test and a jump to a block that loads the field's own
        // error string, and the string names both the class and the field.
        // TradeMarketItemInfo's reader does not match. The DLC build reordered
        // the record (the limit was +0x18 up to 2.03.02, and +0x18 now holds
        // one-byte flags), so a build that reorders it again gets no match
        // rather than a wrong offset.
        //
        //   lea  rdx, [rsi + 0x10]   ; _maxStackCount
        //   mov  rax, [rdi]          ; the stream
        //   mov  r8d, 8
        //   mov  rcx, rdi
        //   call [rax + 0x28]        ; stream->ReadBytes
        //   test al, al / jne        ; then xor ebx, ebx; lea rax, [rip + "ItemInfo... _maxStackCount..."]
        const uintptr_t field = Unique("ItemInfo _maxStackCount read",
            "48 8D 56 10 48 8B 07 41 B8 08 00 00 00 48 8B CF FF 50 28 84 C0 75 ?? 33 DB 48 8D 05 ?? ?? ?? ??");
        if (!field) return false;
        // The message is Korean apart from the two names in it, so it is read as
        // bytes and searched for those. ReadCString would refuse it at the first
        // byte past ASCII.
        char err[96] = {};
        const bool named = mem::ReadBytes(mem::RipAt(field + 25, 7), err, sizeof err - 1) && strncmp(err, "ItemInfo", 8) == 0 &&
                           strstr(err, "_maxStackCount") != nullptr;
        if (!named)
        {
            LOG_ERR("[addr] the eight-byte read at +%llX does not name ItemInfo's _maxStackCount", R(field));
            return false;
        }
        const uintptr_t entry = FunctionEntry(field);
        if (!entry || field - entry > 0x200)
        {
            LOG_ERR("[addr] ItemInfo reader: no function entry near +%llX", R(field));
            return false;
        }
        // (stream in rcx, kept in rdi; record in rdx, kept in rsi).
        if (!mem::MatchAt(entry, "48 8B C4 48 89 58 08 48 89 68 20 56 57 41 56 48 83 EC 60 48 8B F2 48 8B F9"))
        {
            LOG_ERR("[addr] ItemInfo reader at +%llX does not start the way it did on 1.0.0.3392", R(entry));
            return false;
        }
        // _key, four bytes read into a local and stored at record+0x08, which
        // the hook reads to leave the self-refilling ammo alone.
        if (!FindWithin(entry, 0x100,
                        "41 B8 04 00 00 00 48 8D 94 24 ?? ?? ?? ?? 48 8B CF FF 50 28 84 C0 75 ?? 33 DB 48 8D 05 ?? ?? ?? ?? "
                        "E9 ?? ?? ?? ?? 8B 84 24 ?? ?? ?? ?? 89 46 08"))
        {
            LOG_ERR("[addr] ItemInfo reader at +%llX no longer stores _key at +0x08", R(entry));
            return false;
        }
        s.itemInfoRead = entry;
        LOG("[addr] ItemInfo reader +%llX", R(entry));
        return true;
    }
}

#include "storage/capacity.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/paths.h"
#include "core/settings.h"
#include "game/addresses.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "storage/storage.h"

namespace psm::capacity
{
    namespace
    {
        using Fn4 = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

        constexpr int kSlotArray = 1460;   // bucket +0x08 on 2.02; checked against live storage in the dump
        constexpr const wchar_t* kStateFile = L"PrivateStorageMaster.state";

        addr::Capacity g_addr;
        std::atomic<bool> g_stop{false};
        std::atomic<bool> g_dump{false};
        HANDLE g_thread = nullptr;
        void* oRead = nullptr;

        struct Target
        {
            const char* name;
            int storage;          // index into Settings slots
            bool wanted = false;
            // Filled by the first read of the record, on a loader thread.
            std::atomic<bool> seen{false};
            uint16_t stockDefault = 0, stockMax = 0;
            uint8_t needSave = 0;
            uint16_t newDefault = 0, newMax = 0;   // equal to stock when nothing changes
            std::atomic<int> patches{0};
            int logged = 0;
        };
        Target g_targets[] = {
            {"CampWareHouse", 0},
            {"Housing_GatheredMaterials", 1},
            {"Housing_Dresser", 2},
            {"Housing_Refrigerator", 3},
            {"Housing_Collecting", 4},
            {"CampStraw", 5},
            {"BirdFeed", 6},
            {"WareHouse", 7},
            {"Kuku", 8},
        };
        constexpr int kTargetCount = static_cast<int>(sizeof g_targets / sizeof g_targets[0]);

        std::atomic<bool> g_patching{false};
        int g_privateExtras = 0;       // slots the save adds on top of the default, from the state file or the ini
        // The most any save has added. The default is shared by every save, so it
        // has to leave room for the biggest one or that save lands past the slot
        // array. Never lowered by learning; PrivateStorageExpansions overrides it.
        int g_privateExtrasMost = 0;
        bool g_extrasFromIni = false;

        int ReadStateKey(const char* key, int fallback)
        {
            char v[32], def[16];
            snprintf(def, sizeof def, "%d", fallback);
            GetPrivateProfileStringA("PrivateStorageMaster", key, def, v, sizeof v, Paths::FileUtf8(kStateFile).c_str());
            const int n = atoi(v);
            return n >= 0 && n <= kSlotArray ? n : fallback;
        }

        void ReadState()
        {
            g_privateExtras = ReadStateKey("PrivateStorageExtras", 0);
            // State files from 1.0.0 have only the key above.
            const int most = ReadStateKey("PrivateStorageExtrasMost", g_privateExtras);
            g_privateExtrasMost = most > g_privateExtras ? most : g_privateExtras;
        }

        void WriteState()
        {
            const std::string file = Paths::FileUtf8(kStateFile);
            char v[32];
            snprintf(v, sizeof v, "%d", g_privateExtras);
            WritePrivateProfileStringA("PrivateStorageMaster", "PrivateStorageExtras", v, file.c_str());
            snprintf(v, sizeof v, "%d", g_privateExtrasMost);
            WritePrivateProfileStringA("PrivateStorageMaster", "PrivateStorageExtrasMost", v, file.c_str());
        }

        // Sizes from the stock record, per R2 section 6: default and max move by the
        // same amount, so AddExpand clamps where the game had it and saved
        // expansion counts come back unchanged.
        void Plan(Target& t, uint16_t d, uint16_t m, uint8_t needSave)
        {
            t.stockDefault = d;
            t.stockMax = m;
            t.needSave = needSave;
            t.newDefault = d;
            t.newMax = m;
            if (!t.wanted) return;
            const int slots = Settings::Startup().slots[t.storage];
            // Private Storage's setting is a total that counts expansions and story slots.
            // The ceiling leaves room for the most any save has added rather than the
            // last one: a single low reading used to lift the default so far that the
            // next save's own extras went past the slot array (18 September, 2220 of
            // 1460).
            int want = t.storage == 0 ? slots - g_privateExtras : slots;
            const int ceiling = t.storage == 0 ? kSlotArray - g_privateExtrasMost : kSlotArray;
            if (want > ceiling) want = ceiling;
            if (want <= d) return;
            const int delta = want - d;
            t.newDefault = static_cast<uint16_t>(want);
            // Housing chests save no slot count, so their max only has to hold the new
            // default. The rest move max with the default so saved counts stay put.
            if (!needSave) t.newMax = static_cast<uint16_t>(m > want ? m : want);
            else t.newMax = static_cast<uint16_t>(m + delta < kSlotArray ? m + delta : kSlotArray);
        }

        bool RecordName(uintptr_t rec, char* out, size_t cap)
        {
            uintptr_t node = 0, text = 0;
            out[0] = 0;
            return mem::ReadPtr(rec + 8, &node) && mem::ReadPtr(node, &text) && mem::ReadCString(text, out, cap);
        }

        // Loader threads call this. It compares a name and writes two words, and
        // nothing else: no allocation, no locks the game holds, no game calls.
        void PatchRecord(uintptr_t rec)
        {
            char name[40];
            if (!RecordName(rec, name, sizeof name)) return;
            for (Target& t : g_targets)
            {
                if (!t.wanted || strcmp(name, t.name) != 0) continue;
                uint16_t d = 0, m = 0;
                uint8_t needSave = 0;
                if (!mem::Read16(rec + 0x48, &d) || !mem::Read16(rec + 0x4A, &m) || !mem::Read8(rec + 0x9D, &needSave)) return;
                if (!t.seen.load())
                {
                    Plan(t, d, m, needSave);
                    t.seen = true;
                }
                if (t.newDefault == t.stockDefault && t.newMax == t.stockMax) return;
                if (d == t.newDefault && m == t.newMax) return;   // already done
                if (d != t.stockDefault || m != t.stockMax) return; // not the data this plan was made from; leave it
                __try
                {
                    *reinterpret_cast<uint16_t*>(rec + 0x48) = t.newDefault;
                    *reinterpret_cast<uint16_t*>(rec + 0x4A) = t.newMax;
                    ++t.patches;
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                return;
            }
        }

        uintptr_t __fastcall hkRead(uintptr_t stream, uintptr_t rec, uintptr_t r8, uintptr_t r9)
        {
            const uintptr_t r = static_cast<Fn4>(oRead)(stream, rec, r8, r9);
            if ((r & 0xFF) && rec && g_patching.load()) PatchRecord(rec);
            return r;
        }

        // ------------------------------------------------------------ game reads
        uintptr_t Manager(uint32_t* count, uintptr_t* records)
        {
            uintptr_t mgr = 0;
            if (!g_addr.invMgrGlobal || !mem::ReadPtr(g_addr.invMgrGlobal, &mgr)) return 0;
            uintptr_t vt = 0;
            if (!mem::ReadPtr(mgr, &vt) || vt != g_addr.invMgrVtable) return 0;
            if (!mem::Read32(mgr + 0x08, count) || *count == 0 || *count > 256 || !mem::ReadPtr(mgr + 0x58, records)) return 0;
            return mgr;
        }

        bool IndexName(uint16_t index, char* out, size_t cap)
        {
            uint32_t n = 0;
            uintptr_t arr = 0, rec = 0;
            out[0] = 0;
            return Manager(&n, &arr) && index < n && mem::ReadPtr(arr + 8ull * index, &rec) && RecordName(rec, out, cap);
        }

        // Controlled character's inventory holder (R3C 5).
        uintptr_t PlayerHolder()
        {
            uintptr_t g = 0, mgr = 0, user = 0, ch = 0, comp = 0, holder = 0, owner = 0;
            if (!g_addr.actorManagerGlobal || !mem::ReadPtr(g_addr.actorManagerGlobal, &g) || !mem::ReadPtr(g + 0x30, &mgr) ||
                !mem::ReadPtr(mgr + 0x58, &user) || !mem::ReadPtr(user + 0xD8, &ch) || !mem::ReadPtr(ch + 0x68, &comp) ||
                !mem::ReadPtr(comp + 0xB8, &holder))
                return 0;
            if (!mem::ReadPtr(holder + 8, &owner) || owner != ch) return 0;
            return holder;
        }

        struct Bucket { uint16_t index; int16_t cap, requested, granted, story; uint32_t slots; uint32_t filled; bool ok; };

        Bucket ReadBucket(uintptr_t bk, bool countItems)
        {
            Bucket b{};
            uint32_t size = 0;
            uintptr_t slots = 0;
            b.ok = mem::Read16(bk + 0x10, &b.index) && mem::Read32(bk + 0x08, &size) && mem::ReadBytes(bk + 0x14, &b.cap, 2) &&
                   mem::ReadBytes(bk + 0x16, &b.requested, 2) && mem::ReadBytes(bk + 0x18, &b.granted, 2) && mem::ReadBytes(bk + 0x1A, &b.story, 2);
            b.slots = size & 0xFFFF;
            if (b.ok && countItems && mem::ReadPtr(bk, &slots))
            {
                for (uint32_t k = 0; k < b.slots && k < 4096; ++k)
                {
                    uint16_t item = 0xFFFF;
                    int64_t count = 0;
                    if (!mem::Read16(slots + 0xC8ull * k + 0x08, &item) || item == 0xFFFF) continue;
                    if (mem::ReadBytes(slots + 0xC8ull * k + 0x10, &count, sizeof count) && count > 0) ++b.filled;
                }
            }
            return b;
        }

        bool FindBucket(const char* want, Bucket& out)
        {
            const uintptr_t holder = PlayerHolder();
            uintptr_t arr = 0;
            uint32_t n = 0;
            if (!holder || !mem::ReadPtr(holder + 0x18, &arr) || !mem::Read32(holder + 0x20, &n) || n > 128) return false;
            for (uint32_t i = 0; i < n; ++i)
            {
                uintptr_t bk = 0;
                char name[40];
                if (!mem::ReadPtr(arr + 8ull * i, &bk)) continue;
                Bucket b = ReadBucket(bk, false);
                if (!b.ok || !IndexName(b.index, name, sizeof name) || strcmp(name, want) != 0) continue;
                out = b;
                return true;
            }
            return false;
        }

        void Dump()
        {
            uint32_t n = 0;
            uintptr_t arr = 0;
            const uintptr_t mgr = Manager(&n, &arr);
            if (!mgr) { LOG_NOTE("[dump] the inventory table is not loaded yet"); return; }
            LOG_NOTE("[dump] %u inventory records", n);
            for (uint32_t i = 0; i < n; ++i)
            {
                uintptr_t rec = 0;
                char name[40] = "(not loaded)";
                uint16_t d = 0, m = 0;
                if (mem::ReadPtr(arr + 8ull * i, &rec)) { RecordName(rec, name, sizeof name); mem::Read16(rec + 0x48, &d); mem::Read16(rec + 0x4A, &m); }
                const Target* t = nullptr;
                for (const Target& x : g_targets)
                    if (x.seen.load() && strcmp(name, x.name) == 0) t = &x;
                if (t && (t->newDefault != t->stockDefault || t->newMax != t->stockMax))
                    LOG_NOTE("[dump]   record %2u %-26s default %4u max %4u (game data %u and %u)", i, name, d, m, t->stockDefault, t->stockMax);
                else
                    LOG_NOTE("[dump]   record %2u %-26s default %4u max %4u", i, name, d, m);
            }
            const uintptr_t holder = PlayerHolder();
            uintptr_t barr = 0;
            uint32_t bn = 0;
            if (!holder || !mem::ReadPtr(holder + 0x18, &barr) || !mem::Read32(holder + 0x20, &bn) || bn > 128)
            {
                LOG_NOTE("[dump] no character loaded, so no storage to show");
                return;
            }
            LOG_NOTE("[dump] %u storages on the character", bn);
            for (uint32_t i = 0; i < bn; ++i)
            {
                uintptr_t bk = 0;
                if (!mem::ReadPtr(barr + 8ull * i, &bk)) continue;
                const Bucket b = ReadBucket(bk, true);
                if (!b.ok) continue;
                char name[40];
                IndexName(b.index, name, sizeof name);
                LOG_NOTE("[dump]   %-26s %4u of %4d slots used (array %u), expansions bought %d, from story %d%s", name, b.filled, b.cap, b.slots,
                         b.granted, b.story, b.cap > static_cast<int>(b.slots) ? "  CAPACITY IS PAST THE SLOT ARRAY" : "");
            }
        }

        uintptr_t RecordByName(const char* want);
        int s_candidate = -1;
        int s_agreed = 0;

        // With PrivateStorageSlots set and PrivateStorageExpansions=-1, remember what
        // the save adds on top of the default so the next start can hit the total.
        void LearnPrivateExtras()
        {
            // Learned whether or not the size is changed: Master Looter's slider shows the
            // game's own size as the default plus these, and needs them either way.
            const Target& t = g_targets[0];
            if (g_extrasFromIni) return;
            // Only readings taken in free play count, and the hold below has to be
            // unbroken free play. At the title screen Private Storage can sit at its
            // bare default for far longer than the hold: on 18 September a reading
            // taken there saved 0 over a real 760.
            const storage::Play play = storage::PlayState();
            if (play == storage::Play::NotFree) { s_candidate = -1; s_agreed = 0; return; }
            Bucket b{};
            if (!FindBucket("CampWareHouse", b) || b.cap <= 0) return;
            const uintptr_t rec = RecordByName(t.name);
            uint16_t applied = 0;
            if (!rec || !mem::Read16(rec + 0x48, &applied)) return;
            const int extras = b.cap - applied;
            if (extras < 0 || extras > kSlotArray || extras == g_privateExtras) { s_candidate = -1; s_agreed = 0; return; }
            // Without the storage hooks nothing says whether this is free play, so
            // only ever raise the count. Too high a count sizes storage smaller,
            // which is safe. Too low sizes it larger, and that is the direction that
            // runs past the slot array.
            if (play == storage::Play::Unknown && extras < g_privateExtras) { s_candidate = -1; s_agreed = 0; return; }
            // While a save loads, Private Storage sits at its default for several
            // seconds before the expansions are added back, and two reads two seconds
            // apart both saw 0 there. A count is kept once it has held for ten seconds,
            // and a lower count than the one known (another save) only after thirty.
            if (extras != s_candidate) { s_candidate = extras; s_agreed = 1; return; }
            const int need = extras < g_privateExtras ? 15 : 5;
            if (++s_agreed < need) return;
            s_candidate = -1;
            s_agreed = 0;
            g_privateExtras = extras;
            if (extras > g_privateExtrasMost) g_privateExtrasMost = extras;
            // What Plan will give a save with this many extras on the next start.
            const int total = Settings::Startup().slots[0];
            const int roomed = kSlotArray - g_privateExtrasMost + extras;
            if (t.wanted)
                LOG_NOTE("[capacity] Private Storage has %d slots: %d from the default and %d from expansions and story. Saved for the next start, "
                         "which will size it to %d.", b.cap, applied, extras, total < roomed ? total : roomed);
            else
                LOG("[capacity] Private Storage has %d slots, %d of them from expansions and story", b.cap, extras);
            WriteState();
        }

        void FlushLog()
        {
            // A line per storage with DebugLog=1, one line for all of them without.
            int fresh = 0;
            for (Target& t : g_targets)
            {
                const int p = t.patches.load();
                if (!t.seen.load() || p == t.logged) continue;
                if (!t.logged)
                {
                    LOG("[capacity] %s: default %u -> %u, max %u -> %u", t.name, t.stockDefault, t.newDefault, t.stockMax, t.newMax);
                    ++fresh;
                }
                else
                    LOG("[capacity] %s read again by the game and set again (%d times)", t.name, p);
                t.logged = p;
            }
            if (fresh && !Log::Debug())
                LOG_NOTE("[capacity] sizes set for %d storage%s; DebugLog=1 lists each one", fresh, fresh == 1 ? "" : "s");
        }

        // ------------------------------------------------------------ size table for Master Looter
        SRWLOCK g_sizeLock = SRWLOCK_INIT;
        SizeInfo g_sizes[Settings::kStorages];
        std::atomic<DWORD> g_sizesWanted{0};
        std::atomic<bool> g_hooked{false};

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


        // ------------------------------------------------------------ deposit probe (R5)
        // Takes the next unlocked stack in the bag and offers it to each storage in
        // turn through the game's own move check, stopping at the first that sends.
        using MoveFn = void(__fastcall*)(uintptr_t holder, uint32_t* err, uint32_t actorA, uint32_t actorB, uint16_t item,
                                         uint16_t variant, uint64_t count, uint16_t fromInventory, uint16_t srcSlot, uint32_t moveIndex);

        constexpr const char* kProbeOrder[] = {"Housing_Collecting", "Housing_GatheredMaterials", "Housing_Refrigerator", "Housing_Dresser",
                                                "BirdFeed", "CampStraw", "Kuku", "CampWareHouse"};

        std::atomic<bool> g_probeWanted{false};
        DWORD g_probeCheckAt = 0;
        uint32_t g_probeNextSlot = 0;
        struct ProbeMove
        {
            uint16_t item, slot, bagIndex, targetIndex;
            int64_t bagBefore;
            uint32_t targetBefore;
            char target[40];
        } g_probeMove{};

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

        uintptr_t PlayerCharacter()
        {
            uintptr_t g = 0, mgr = 0, user = 0, ch = 0;
            if (!g_addr.actorManagerGlobal || !mem::ReadPtr(g_addr.actorManagerGlobal, &g) || !mem::ReadPtr(g + 0x30, &mgr) ||
                !mem::ReadPtr(mgr + 0x58, &user) || !mem::ReadPtr(user + 0xD8, &ch))
                return 0;
            return ch;
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

        // Index of the plain (type 0) entry from -> to in a record's move list (R3C 1.1).
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

        bool SlotAt(uintptr_t bucket, uint32_t k, uint16_t* item, uint16_t* variant, int64_t* count, uint8_t* locked, uint64_t* instance)
        {
            uintptr_t slots = 0;
            const uintptr_t s = mem::ReadPtr(bucket, &slots) ? slots + 0xC8ull * k : 0;
            return s && mem::Read16(s + 0x08, item) && mem::Read16(s + 0x0A, variant) && mem::ReadBytes(s + 0x10, count, sizeof *count) &&
                   mem::Read8(s + 0xA1, locked) && mem::Read64(s, instance);
        }

        bool CallMove(uintptr_t fn, uintptr_t holder, uint32_t* err, uint32_t actor, uint16_t item, uint16_t variant, uint64_t count,
                      uint16_t from, uint16_t slot, uint32_t moveIndex)
        {
            __try
            {
                reinterpret_cast<MoveFn>(fn)(holder, err, actor, actor, item, variant, count, from, slot, moveIndex);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        void RunDepositProbe()
        {
            if (!g_addr.clientMoveItem) { LOG_NOTE("[probe] the move function was not found, so there is nothing to test"); return; }
            const uintptr_t ch = PlayerCharacter();
            const uintptr_t holder = PlayerHolder();
            uint32_t actor = 0;
            const int bagIndex = IndexByName("Character");
            const uintptr_t bagRecord = RecordByName("Character");
            const uintptr_t bag = bagIndex >= 0 ? BucketByIndex(holder, static_cast<uint16_t>(bagIndex)) : 0;
            if (!ch || !holder || !mem::Read32(ch + 0x60, &actor) || !bagRecord || !bag)
            {
                LOG_NOTE("[probe] no character, bag or inventory table (character %s, holder %s, actor 0x%08X, bag index %d)",
                         ch ? "yes" : "no", holder ? "yes" : "no", actor, bagIndex);
                return;
            }
            uint32_t size = 0;
            mem::Read32(bag + 0x08, &size);
            size &= 0xFFFF;

            uint16_t item = 0xFFFF, variant = 0;
            int64_t count = 0;
            uint8_t locked = 0;
            uint64_t instance = 0;
            uint32_t slot = size;
            for (uint32_t step = 0; step < size; ++step)
            {
                const uint32_t k = (g_probeNextSlot + step) % size;
                if (SlotAt(bag, k, &item, &variant, &count, &locked, &instance) && item != 0xFFFF && count > 0 && !locked) { slot = k; break; }
            }
            if (slot == size) { LOG_NOTE("[probe] no unlocked item in the bag"); return; }
            g_probeNextSlot = slot + 1;
            LOG_NOTE("[probe] bag slot %u: item %u variant 0x%04X count %lld instance %llu, actor 0x%08X, bag record index %d",
                     slot, item, variant, static_cast<long long>(count), static_cast<unsigned long long>(instance), actor, bagIndex);

            for (const char* name : kProbeOrder)
            {
                const int idx = IndexByName(name);
                if (idx < 0) { LOG_NOTE("[probe]   %-26s no such record", name); continue; }
                const uintptr_t bucket = BucketByIndex(holder, static_cast<uint16_t>(idx));
                const Bucket before = bucket ? ReadBucket(bucket, true) : Bucket{};
                const int mi = MoveIndex(bagRecord, static_cast<uint16_t>(bagIndex), static_cast<uint16_t>(idx));
                if (!bucket || mi < 0) { LOG_NOTE("[probe]   %-26s skipped (bucket %s, move entry %d)", name, bucket ? "yes" : "no", mi); continue; }
                uint32_t err = 0xFFFFFFFF;
                if (!CallMove(g_addr.clientMoveItem, holder, &err, actor, item, variant, static_cast<uint64_t>(count),
                              static_cast<uint16_t>(bagIndex), static_cast<uint16_t>(slot), static_cast<uint32_t>(mi)))
                {
                    LOG_ERR("[probe]   %-26s the move function raised an exception; stopping", name);
                    return;
                }
                LOG_NOTE("[probe]   %-26s move entry %2d, %u of %d used: %s (0x%08X)", name, mi, before.filled, before.cap, ErrName(err), err);
                if (err == 0)
                {
                    g_probeMove = ProbeMove{item, static_cast<uint16_t>(slot), static_cast<uint16_t>(bagIndex), static_cast<uint16_t>(idx), count, before.filled, {}};
                    strncpy_s(g_probeMove.target, name, _TRUNCATE);
                    g_probeCheckAt = GetTickCount() + 500;
                    return;
                }
            }
            LOG_NOTE("[probe] no storage took item %u", item);
        }

        void CheckDepositProbe()
        {
            g_probeCheckAt = 0;
            const uintptr_t holder = PlayerHolder();
            const uintptr_t bag = BucketByIndex(holder, g_probeMove.bagIndex);
            const uintptr_t target = BucketByIndex(holder, g_probeMove.targetIndex);
            uint16_t item = 0xFFFF, variant = 0;
            int64_t count = 0;
            uint8_t locked = 0;
            uint64_t instance = 0;
            const bool slotRead = bag && SlotAt(bag, g_probeMove.slot, &item, &variant, &count, &locked, &instance);
            const Bucket after = target ? ReadBucket(target, true) : Bucket{};
            LOG_NOTE("[probe] 500 ms later: bag slot %u holds item %u count %lld (was item %u count %lld); %s has %u used (was %u)",
                     g_probeMove.slot, slotRead ? item : 0xFFFF, static_cast<long long>(slotRead ? count : 0), g_probeMove.item,
                     static_cast<long long>(g_probeMove.bagBefore), g_probeMove.target, after.filled, g_probeMove.targetBefore);
        }

        void RefreshSizes()
        {
            SizeInfo fresh[Settings::kStorages]{};
            for (const Target& t : g_targets)
            {
                SizeInfo& o = fresh[t.storage];
                o.liveCapacity = -1;
                o.liveUsed = -1;
                if (const uintptr_t rec = RecordByName(t.name))
                {
                    uint16_t d = 0, m = 0;
                    if (mem::Read16(rec + 0x48, &d) && mem::Read16(rec + 0x4A, &m))
                    {
                        o.known = true;
                        o.appliedDefault = d;
                        const bool changed = t.seen.load() && t.patches.load() > 0;
                        o.gameDefault = changed ? t.stockDefault : d;
                        o.gameMax = changed ? t.stockMax : m;
                    }
                }
                Bucket b{};
                if (FindBucket(t.name, b))
                {
                    o.liveCapacity = b.cap;
                    o.slotArray = static_cast<int>(b.slots);
                    o.extras = b.granted + b.story;
                    uintptr_t holder = PlayerHolder(), arr = 0, bk = 0;
                    uint32_t n = 0;
                    if (holder && mem::ReadPtr(holder + 0x18, &arr) && mem::Read32(holder + 0x20, &n) && n <= 128)
                        for (uint32_t i = 0; i < n; ++i)
                        {
                            char name[40];
                            if (!mem::ReadPtr(arr + 8ull * i, &bk)) continue;
                            const Bucket full = ReadBucket(bk, true);
                            if (full.ok && IndexName(full.index, name, sizeof name) && strcmp(name, t.name) == 0) { o.liveUsed = static_cast<int>(full.filled); break; }
                        }
                }
            }
            AcquireSRWLockExclusive(&g_sizeLock);
            memcpy(g_sizes, fresh, sizeof fresh);
            ReleaseSRWLockExclusive(&g_sizeLock);
        }

        DWORD WINAPI Worker(LPVOID)
        {
            DWORD nextLearn = 0, nextSizes = 0;
            while (!g_stop.load())
            {
                Sleep(100);
                if (g_dump.exchange(false)) Dump();
                const DWORD tick = GetTickCount();
                if (tick - g_sizesWanted.load() < 3000 && tick >= nextSizes) { RefreshSizes(); nextSizes = tick + 1000; }
                const DWORD now = GetTickCount();
                if (now < nextLearn) continue;
                nextLearn = now + 2000;
                FlushLog();
                LearnPrivateExtras();
            }
            return 0;
        }

        // Records the game already read before the hook went in.
        void PatchLoaded()
        {
            uint32_t n = 0;
            uintptr_t arr = 0;
            if (!Manager(&n, &arr)) return;
            for (uint32_t i = 0; i < n; ++i)
            {
                uintptr_t rec = 0;
                if (mem::ReadPtr(arr + 8ull * i, &rec)) PatchRecord(rec);
            }
        }
    }

    void Start()
    {
        const Settings::Values& v = Settings::Startup();
        bool any = false;
        if (!v.leaveCapacityAlone)
        {
            for (Target& t : g_targets)
            {
                t.wanted = v.slots[t.storage] > 0;
                any |= t.wanted;
            }
        }
        if (v.privateStorageExpansions >= 0)
        {
            g_privateExtras = v.privateStorageExpansions < kSlotArray ? v.privateStorageExpansions : kSlotArray;
            g_privateExtrasMost = g_privateExtras;   // the player said exactly; trust it
            g_extrasFromIni = true;
        }
        else ReadState();

        // The game can take a while to unpack its code; keep trying for a minute.
        bool resolved = false;
        for (int i = 0; i < 120 && !g_stop.load(); ++i)
        {
            if ((resolved = addr::ResolveCapacity(g_addr)) != false) break;
            if (i == 0) LOG("[capacity] addresses not found yet, retrying");
            Sleep(500);
        }
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (!any)
        {
            LOG_NOTE("[capacity] %s", v.leaveCapacityAlone ? "LeaveCapacityAlone=1: no storage size is changed"
                                                            : "every Slots setting is 0: no storage size is changed");
            return;
        }
        if (!resolved || !g_addr.inventoryInfoRead)
        {
            LOG_ERR("[capacity] the inventory table reader was not found in this game version, so storage sizes are left alone");
            return;
        }
        // A save already loaded has built its storage from the stock records.
        // Changing them under it is what R2 warns against, so wait for a restart.
        if (PlayerHolder())
        {
            LOG_ERR("[capacity] a save was already loaded when the mod started, so storage sizes change from the next start");
            return;
        }
        char why[128];
        if (!farhook::Install("InventoryInfoRead", g_addr.inventoryInfoRead, hkRead, &oRead, why, sizeof why))
        {
            LOG_ERR("[capacity] hooking the inventory table reader failed: %s. Storage sizes are left alone.", why);
            return;
        }
        g_patching = true;
        g_hooked = true;
        PatchLoaded();
        if (v.slots[0] > 0)
        {
            LOG_NOTE("[capacity] Private Storage target %d slots, counting %d from expansions and story (%s)", v.slots[0],
                     g_privateExtras, g_extrasFromIni ? "PrivateStorageExpansions" : "learned from the save last time");
            if (g_privateExtrasMost > g_privateExtras)
                LOG_NOTE("[capacity] Private Storage leaves room for %d extra slots, the most any save has had, so no save goes past the "
                         "%d-slot array. Set PrivateStorageExpansions to override.", g_privateExtrasMost, kSlotArray);
        }
        FlushLog();
    }

    void RequestDump() { g_dump = true; }

    bool Hooked() { return g_hooked.load(); }

    int LearnedPrivateExtras() { return g_privateExtras; }

    void GetSize(int storage, SizeInfo& out)
    {
        g_sizesWanted = GetTickCount();
        out = SizeInfo{};
        if (storage < 0 || storage >= Settings::kStorages) return;
        AcquireSRWLockShared(&g_sizeLock);
        out = g_sizes[storage];
        ReleaseSRWLockShared(&g_sizeLock);
    }

    void RequestDepositProbe() { g_probeWanted = true; }

    void DepositProbeTick()
    {
        if (g_probeWanted.exchange(false)) RunDepositProbe();
        if (g_probeCheckAt && GetTickCount() >= g_probeCheckAt) CheckDepositProbe();
    }

    void Stop()
    {
        g_stop = true;
        if (g_thread)
        {
            WaitForSingleObject(g_thread, 1000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
    }
}

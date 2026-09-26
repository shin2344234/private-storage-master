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
#include "storage/inventory.h"
#include "storage/stacks.h"
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

        using inv::IndexName;
        using inv::Manager;
        using inv::PlayerHolder;
        using inv::RecordByName;
        using inv::RecordName;

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
        // Slots the save adds on top of the default, from the state file or the ini.
        // Only Master Looter's slider reads it now: the size no longer depends on it.
        int g_privateExtras = 0;
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
            // PrivateStorageExtrasMost, written from 1.0.1 to 1.1.4, is no longer read.
            g_privateExtras = ReadStateKey("PrivateStorageExtras", 0);
        }

        void WriteState()
        {
            const std::string file = Paths::FileUtf8(kStateFile);
            char v[32];
            snprintf(v, sizeof v, "%d", g_privateExtras);
            WritePrivateProfileStringA("PrivateStorageMaster", "PrivateStorageExtras", v, file.c_str());
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
            int want = Settings::Startup().slots[t.storage];
            if (want > kSlotArray) want = kSlotArray;
            if (want <= d) return;
            if (t.storage == 0)
            {
                // Private Storage is the setting for every save, whatever the save
                // adds. Its extra slots come from the story alone, through SetExpand,
                // which sets capacity to the default plus the stage's slots with no
                // clamp and saves nothing (R2 section 3); GameTick trims that back.
                // Max is the same number, so a later AddExpand finds capacity at max
                // and grows by 0, and the saved count (+0x18) never moves.
                t.newDefault = t.newMax = static_cast<uint16_t>(want);
                return;
            }
            const int delta = want - d;
            t.newDefault = static_cast<uint16_t>(want);
            // Housing chests save no slot count, so their max only has to hold the new
            // default. The rest move max with the default so saved counts stay put.
            if (!needSave) t.newMax = static_cast<uint16_t>(m > want ? m : want);
            else t.newMax = static_cast<uint16_t>(m + delta < kSlotArray ? m + delta : kSlotArray);
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

        bool FindBucket(const char* want, Bucket& out, uintptr_t* at = nullptr)
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
                if (at) *at = bk;
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
            const stacks::Report st = stacks::Status();
            if (st.reason == stacks::kApplying)
                LOG_NOTE("[dump] stacks x%d: %d items raised, the biggest now %lld, %d items the game does not stack left alone", st.multiplier,
                         st.patched, static_cast<long long>(st.biggest), st.unstackable);
            else if (st.reason == stacks::kOtherMod)
                LOG_NOTE("[dump] stacks: Stack Master is installed and sets them");
            else if (st.reason != stacks::kOff)
                LOG_NOTE("[dump] stacks: asked for, but not applied (reason %d); see the errors above", static_cast<int>(st.reason));
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
            // The count the game asked for (+0x16), which AddExpand adds to and the
            // story's SetExpand sets, rather than capacity less the default: GameTick
            // trims capacity, and a trimmed reading is not the save's count.
            const int extras = b.requested;
            if (extras < 0 || extras > kSlotArray || extras == g_privateExtras) { s_candidate = -1; s_agreed = 0; return; }
            // Without the storage hooks nothing says whether this is free play, so
            // only ever raise the count.
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
            if (t.wanted)
                LOG_NOTE("[capacity] Private Storage has %d slots: %d from the default and %d from expansions and story. Saved for the next start.",
                         b.cap, applied, extras);
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
                // The frame tick trims Private Storage; without the storage hooks
                // there is no frame tick, so this thread does it instead.
                if (!storage::Ready()) GameTick();
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
        inv::SetAddresses(g_addr);   // deposits and the size table read through these
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
            LOG_NOTE("[capacity] Private Storage target %d slots for every save; story slots on top are trimmed back in play", v.slots[0]);
        FlushLog();
    }

    // Private Storage is the one storage the game raises with no clamp: the
    // story's SetExpand sets capacity to the default plus the stage's slots (R2
    // section 3, writer 5), which would put it above the setting and can put it
    // past the slot array. Capacity goes back to the setting, only ever down and
    // never below what the game alone would give. It is not saved, and with max
    // at the setting AddExpand finds capacity at or above max and grows by 0, so
    // the saved expansion count (+0x18) does not move.
    void GameTick()
    {
        static ULONGLONG s_next = 0;
        static int s_trims = 0;
        const Target& t = g_targets[0];
        if (!g_hooked.load() || !t.wanted || t.patches.load() == 0) return;
        const ULONGLONG now = GetTickCount64();
        if (now < s_next) return;
        s_next = now + 1000;
        Bucket b{};
        uintptr_t bk = 0;
        if (!FindBucket("CampWareHouse", b, &bk) || b.slots == 0 || b.slots > kSlotArray) return;
        int keep = t.newDefault;
        const int stock = t.stockDefault + (b.requested > 0 ? b.requested : 0);
        const int game = stock < t.stockMax ? stock : t.stockMax;
        if (game > keep) keep = game;
        if (keep > static_cast<int>(b.slots)) keep = static_cast<int>(b.slots);
        if (b.cap <= keep) return;
        __try
        {
            *reinterpret_cast<int16_t*>(bk + 0x14) = static_cast<int16_t>(keep);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        if (++s_trims == 1)
            LOG_NOTE("[capacity] Private Storage came to %d slots with %d from the story; set back to %d", b.cap, b.story, keep);
        else
            LOG("[capacity] Private Storage set back from %d to %d slots (%d times)", b.cap, keep, s_trims);
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

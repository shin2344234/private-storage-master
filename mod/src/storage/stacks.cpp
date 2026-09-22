#include "storage/stacks.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>

#include "core/log.h"
#include "core/settings.h"
#include "game/addresses.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "storage/deposit.h"

namespace psm::stacks
{
    namespace
    {
        using Fn4 = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

        // ItemInfo, from R9 section 2.
        constexpr unsigned kMaxStackCount = 0x18;   // eight bytes
        constexpr unsigned kKey = 0x00;             // four bytes, the item number
        // No stack is taken past this. Fat Stacks' largest option is 999999 and
        // the game carries it in this same field, so it is a number the game is
        // known to cope with. A stock limit already at or past it is left alone:
        // money sits far above it and clamping would shrink it.
        constexpr int64_t kCeiling = 999999;
        // Replenishing Arrows, Bullets and Cannonballs refill themselves. A player
        // reported their Replenishing Arrows running out with the limit raised, and
        // the refill most likely goes by that limit, so these keep the game's own.
        constexpr uint32_t kKeepStock[] = {1002557, 1003753, 1003752};
        // The standalone mod. Seth's decision is that it wins when both are
        // installed, so this one stands down rather than stacking the multipliers.
        constexpr const wchar_t* kOtherProvider = L"StackMaster.asi";

        // Every record the hook raised, so a later multiplier can be worked out
        // from the game's own limit rather than from a number already multiplied
        // (the ceiling makes that ratio wrong). Fixed size and filled by loader
        // threads, which must not allocate: 2397 records were raised on 2.03.00,
        // so this is roughly three times the room needed.
        constexpr int kMaxEntries = 8192;
        struct Entry
        {
            uintptr_t rec;
            int64_t stock;     // the game's own limit
            int64_t written;   // what the mod last wrote there
        };
        Entry g_entries[kMaxEntries];
        std::atomic<int> g_entryCount{0};
        std::atomic<bool> g_entriesFull{false};

        addr::Stacks g_addr;
        void* oRead = nullptr;
        std::atomic<int> g_multiplier{1};
        std::atomic<bool> g_stop{false};
        std::atomic<bool> g_patching{false};
        std::atomic<bool> g_hooked{false};
        std::atomic<int> g_reason{kOff};
        std::atomic<int> g_patched{0};
        std::atomic<int64_t> g_biggest{0};
        std::atomic<int> g_unstackable{0};
        std::atomic<int> g_huge{0};
        std::atomic<int> g_logged{0};

        // Stack Master loaded in this process. The export decides, not the name
        // alone, so a renamed or half-installed file is not mistaken for it.
        bool OtherProviderLoaded()
        {
            const HMODULE h = GetModuleHandleW(kOtherProvider);
            return h && GetProcAddress(h, "StackGetStatus") != nullptr;
        }

        // Loader threads call this. It reads two fields and writes one, and does
        // nothing else: no allocation, no lock, no game call. The stream fills the
        // record from the data file immediately before this runs, so the value
        // read here is always the game's own and a record cannot be raised twice.
        void PatchRecord(uintptr_t rec)
        {
            int64_t stock = 0;
            if (!mem::ReadBytes(rec + kMaxStackCount, &stock, sizeof stock)) return;
            // 0 and 1 are the game saying this item does not stack. Multiplying
            // those would let gear, quest items and mounts pile into one slot.
            if (stock <= 1) { ++g_unstackable; return; }
            if (stock >= kCeiling) { ++g_huge; return; }
            uint32_t key = 0;
            if (!mem::Read32(rec + kKey, &key)) return;
            for (const uint32_t k : kKeepStock)
                if (key == k) return;
            int64_t want = stock * g_multiplier.load();
            if (want > kCeiling) want = kCeiling;
            if (want <= stock) return;
            __try
            {
                *reinterpret_cast<int64_t*>(rec + kMaxStackCount) = want;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return; }
            // Remembered so RaiseNow can recompute from `stock`. A record the game
            // reads again lands here again, and the entry is updated rather than
            // repeated, so the list cannot drift from what the records hold.
            const int have = g_entryCount.load();
            int slot = -1;
            for (int i = 0; i < have; ++i)
                if (g_entries[i].rec == rec) { slot = i; break; }
            if (slot < 0)
            {
                slot = g_entryCount.fetch_add(1);
                if (slot >= kMaxEntries)
                {
                    g_entryCount.store(kMaxEntries);
                    g_entriesFull = true;
                    slot = -1;
                }
            }
            if (slot >= 0) g_entries[slot] = Entry{rec, stock, want};
            ++g_patched;
            if (want > g_biggest.load()) g_biggest = want;
            if (Log::Debug() && g_logged.fetch_add(1) < 10)
            {
                LOG("[stacks] item %u stacks to %lld instead of %lld", key, static_cast<long long>(want), static_cast<long long>(stock));
            }
        }

        uintptr_t __fastcall hkRead(uintptr_t stream, uintptr_t rec, uintptr_t r8, uintptr_t r9)
        {
            const uintptr_t r = static_cast<Fn4>(oRead)(stream, rec, r8, r9);
            if ((r & 0xFF) && rec && g_patching.load()) PatchRecord(rec);
            return r;
        }
    }

    void Start()
    {
        const int mult = Settings::Startup().stackMultiplier;
        if (mult <= 1)
        {
            g_reason = kOff;
            LOG("[stacks] StackMultiplier=1, so stack sizes are the game's own");
            return;
        }
        g_multiplier = mult;
        if (OtherProviderLoaded())
        {
            g_reason = kOtherMod;
            LOG_NOTE("[stacks] Stack Master is installed, so it sets the stack sizes and this mod leaves them alone. "
                     "Change the multiplier in StackMaster.ini, not here.");
            return;
        }
        // The same retry as capacity: early in a launch the game has not finished
        // unpacking its code and no pattern matches yet. The item table is read
        // later than that, so a hook that waits here still lands before it.
        bool resolved = false;
        for (int i = 0; i < 120 && !g_stop.load(); ++i)
        {
            if ((resolved = addr::ResolveStacks(g_addr)) != false) break;
            if (i == 0) LOG("[stacks] the item table reader was not found yet, retrying");
            Sleep(500);
        }
        if (!resolved)
        {
            g_reason = kNoAnchor;
            LOG_ERR("[stacks] the item table reader was not found in this game version, so stack sizes are left alone");
            return;
        }
        // The ASI loader may not have reached StackMaster.asi when this started,
        // so look again now. The item table is still seconds away at this point,
        // which is what makes standing down here safe.
        Sleep(1500);
        if (g_stop.load()) return;
        if (OtherProviderLoaded())
        {
            g_reason = kOtherMod;
            LOG_NOTE("[stacks] Stack Master loaded as well, so it sets the stack sizes and this mod leaves them alone");
            return;
        }
        char why[128];
        if (!farhook::Install("ItemInfoRead", g_addr.itemInfoRead, hkRead, &oRead, why, sizeof why))
        {
            g_reason = kHookFailed;
            LOG_ERR("[stacks] hooking the item table reader failed: %s. Stack sizes are left alone.", why);
            return;
        }
        g_patching = true;
        g_hooked = true;
        g_reason = kApplying;
        LOG_NOTE("[stacks] StackMultiplier=%d: every item that already stacks holds %d times as much, up to %lld", mult, mult,
                 static_cast<long long>(kCeiling));
    }

    void Flush()
    {
        const int mult = g_multiplier.load();
        if (mult <= 1 || !g_hooked.load()) return;
        const int n = g_patched.load();
        if (n)
            LOG_NOTE("[stacks] %d items stack x%d now, the biggest holding %lld; %d items the game does not stack were left alone", n, mult,
                     static_cast<long long>(g_biggest.load()), g_unstackable.load());
        else
        {
            g_reason = kTooLate;
            LOG_ERR("[stacks] no item was raised, so the game read its item table before the mod started. Stack sizes are the game's this session.");
        }
    }

    bool CanRaiseNow() { return g_hooked.load() && g_reason.load() == kApplying; }

    bool RaiseNow(int multiplier, char* why, size_t whyLen)
    {
        const auto no = [&](const char* text) { if (why && whyLen) snprintf(why, whyLen, "%s", text); return false; };
        if (!CanRaiseNow())
            return no("stacks are not being changed at all this session");
        const int now = g_multiplier.load();
        if (multiplier <= now)
            return no("a smaller multiplier cannot touch stacks already built, because a slot holding more than the game "
                      "allows would be left stranded over the limit");
        // The same gate the deposit path uses. A storage or inventory screen reads
        // these limits when it opens, so changing them underneath one is asking
        // for a screen that disagrees with the data.
        if (!deposit::FreePlayNow())
            return no("raising a stack size needs free play with no storage screen open");
        const int count = g_entryCount.load();
        int changed = 0, skipped = 0;
        int64_t biggest = 0;
        for (int i = 0; i < count && i < kMaxEntries; ++i)
        {
            Entry& e = g_entries[i];
            int64_t want = e.stock * multiplier;
            if (want > kCeiling) want = kCeiling;
            if (want <= e.written) continue;   // already there, ceiling reached
            int64_t live = 0;
            // Only a record still holding exactly what this mod wrote is touched.
            // Anything else is the game's or another mod's now, and is left alone.
            if (!mem::ReadBytes(e.rec + kMaxStackCount, &live, sizeof live) || live != e.written) { ++skipped; continue; }
            __try
            {
                // Eight bytes, aligned, so a reader on the game's thread sees the
                // old limit or the new one and never half of either.
                *reinterpret_cast<int64_t*>(e.rec + kMaxStackCount) = want;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { ++skipped; continue; }
            e.written = want;
            if (want > biggest) biggest = want;
            ++changed;
        }
        // Records the game reads after this get the new multiplier too.
        g_multiplier.store(multiplier);
        if (biggest > g_biggest.load()) g_biggest = biggest;
        LOG_NOTE("[stacks] raised to x%d without a restart: %d items changed, the biggest now %lld%s", multiplier, changed,
                 static_cast<long long>(biggest), skipped ? ", some left alone because something else had changed them" : "");
        if (skipped) LOG("[stacks] %d records were not what the mod last wrote, so they were left alone", skipped);
        if (g_entriesFull.load())
            LOG_ERR("[stacks] more than %d items were raised at startup, so the ones past that keep the old multiplier until the next launch",
                    kMaxEntries);
        return true;
    }

    Report Status()
    {
        Report r;
        r.hooked = g_hooked.load();
        r.reason = static_cast<Reason>(g_reason.load());
        r.multiplier = r.reason == kApplying ? g_multiplier.load() : 1;
        r.patched = g_patched.load();
        r.biggest = g_biggest.load();
        r.unstackable = g_unstackable.load();
        return r;
    }

    void Stop()
    {
        // The hook itself is removed with all the others by farhook::RemoveAll.
        g_stop = true;
        g_patching = false;
    }
}

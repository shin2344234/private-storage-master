#include "storage/deposit.h"

#include <Windows.h>
#include <cstring>

#include "core/log.h"
#include "core/settings.h"
#include "game/mem.h"
#include "storage/inventory.h"

namespace psm::deposit
{
    namespace
    {
        // The order storages are offered an item. The Collectibles Chest and Abyss
        // gear storage take only their own things, so they go first. The camp bins
        // come after the Kuku Cooler: Camp Straw also takes cooked food and drinks
        // (Honey Tea, Harvest Roll; 18 September on 2.03.00), and feeding the whole
        // food bag into a 50-slot bin is not what anyone wants. Private Storage,
        // which takes almost anything, is last.
        constexpr int kOrder[] = {4, 8, 1, 3, 6, 5, 2, 0};
        constexpr int kOrderCount = static_cast<int>(sizeof kOrder / sizeof kOrder[0]);
        constexpr int kCollecting = 4;

        constexpr DWORD kWaitForBagMs = 1500;   // a pickup is its own server request
        constexpr DWORD kConfirmMs = 1000;
        constexpr DWORD kFullForMs = 10000;     // a full storage is not asked again for this long
        constexpr int kSendsPerFrame = 4;

        // ---------------------------------------------------------------- incoming queue, any thread
        struct Request { uint16_t item; int64_t gained; bool debug; };
        constexpr int kQueueMax = 128;
        SRWLOCK g_queueLock = SRWLOCK_INIT;
        Request g_queue[kQueueMax];
        int g_queueCount = 0;

        bool Push(const Request& r)
        {
            AcquireSRWLockExclusive(&g_queueLock);
            const bool room = g_queueCount < kQueueMax;
            if (room) g_queue[g_queueCount++] = r;
            ReleaseSRWLockExclusive(&g_queueLock);
            return room;
        }

        // ---------------------------------------------------------------- results, any thread
        constexpr int kResultMax = 128;
        SRWLOCK g_resultLock = SRWLOCK_INIT;
        Result g_results[kResultMax];
        int g_resultHead = 0, g_resultCount = 0;

        void Finish(uint16_t item, int storage, Reason reason, int64_t moved)
        {
            static const char* const kReasonText[] = {"", "stored", "no storage takes it", "every storage that takes it is full", "on the never-move list",
                                                      "locked", "not in the bag", "auto-store is off", "too many at once"};
            LOG("[deposit] item %u: %s%s%s, %lld moved", item, kReasonText[reason], storage >= 0 ? " in " : "",
                storage >= 0 ? Settings::StorageLabel(storage) : "", static_cast<long long>(moved));
            AcquireSRWLockExclusive(&g_resultLock);
            g_results[(g_resultHead + g_resultCount) % kResultMax] = Result{item, static_cast<int16_t>(storage), reason, moved};
            if (g_resultCount < kResultMax) ++g_resultCount;
            else g_resultHead = (g_resultHead + 1) % kResultMax;   // the oldest is dropped
            ReleaseSRWLockExclusive(&g_resultLock);
        }

        // ---------------------------------------------------------------- jobs, game thread only
        enum class Phase { Find, Confirm };
        struct Job
        {
            uint16_t item = 0xFFFF;    // 0xFFFF: finished, removed at the end of the frame
            int64_t left = 0;          // still to move
            int64_t moved = 0;
            int lastStorage = -1;      // where the last part went
            bool debug = false;
            Phase phase = Phase::Find;
            DWORD waitingSince = 0;    // Find: when the wait for the bag began
            int nextOrder = 0;         // first kOrder position still to try
            bool sawFull = false;
            // Confirm
            int storage = -1;
            uintptr_t target = 0;      // the storage's bucket
            int64_t targetBefore = 0;  // how many of the item it held before the send
            int64_t amount = 0;
            DWORD sentAt = 0;
        };
        constexpr int kJobMax = 64;
        Job g_jobs[kJobMax];
        int g_jobCount = 0;

        // Per storage: full until this tick. Per item and storage: the server refused
        // it once this session, so it is not offered there again.
        DWORD g_fullUntil[Settings::kStorages] = {};
        struct Refused { uint16_t item; int8_t storage; };
        constexpr int kRefusedMax = 256;
        Refused g_refused[kRefusedMax];
        int g_refusedCount = 0, g_refusedNext = 0;
        bool g_broken = false;   // the move function raised; nothing is sent again this session

        bool WasRefused(uint16_t item, int storage)
        {
            for (int i = 0; i < g_refusedCount; ++i)
                if (g_refused[i].item == item && g_refused[i].storage == storage) return true;
            return false;
        }

        void RememberRefused(uint16_t item, int storage)
        {
            if (WasRefused(item, storage)) return;
            g_refused[g_refusedNext] = Refused{item, static_cast<int8_t>(storage)};
            g_refusedNext = (g_refusedNext + 1) % kRefusedMax;
            if (g_refusedCount < kRefusedMax) ++g_refusedCount;
        }

        bool NeverMove(const Settings::Values& v, uint16_t item)
        {
            for (int i = 0; i < v.autoStoreNeverMoveCount; ++i)
                if (v.autoStoreNeverMove[i] == item) return true;
            return false;
        }

        // Another job has a move of this item on the wire; the bag is not settled.
        bool ItemInFlight(uint16_t item, const Job* except)
        {
            for (int i = 0; i < g_jobCount; ++i)
                if (&g_jobs[i] != except && g_jobs[i].item == item && g_jobs[i].phase == Phase::Confirm) return true;
            return false;
        }

        struct Bag
        {
            uintptr_t holder = 0;
            uintptr_t bucket = 0;
            uintptr_t record = 0;
            int index = -1;
            uint32_t actor = 0;   // the key the move is sent for (R5, character +0x60)
        };

        bool ReadBag(Bag& b)
        {
            const uintptr_t ch = inv::PlayerCharacter();
            b.holder = inv::PlayerHolder();
            b.index = inv::IndexByName("Character");
            b.record = inv::RecordByName("Character");
            b.bucket = b.index >= 0 ? inv::BucketByIndex(b.holder, static_cast<uint16_t>(b.index)) : 0;
            return ch && b.holder && b.record && b.bucket && mem::Read32(ch + 0x60, &b.actor) && b.actor != 0;
        }

        // The newest unlocked stack of the item. Instance ids grow as stacks are
        // made, so the highest one is what just arrived when a pickup made a new
        // stack, and the one it joined when it merged.
        bool PickSlot(const Bag& b, uint16_t item, uint32_t& slot, inv::Slot& s, bool& onlyLocked)
        {
            bool found = false, lockedSeen = false;
            const uint32_t size = inv::SlotCount(b.bucket);
            inv::Slot cur;
            for (uint32_t k = 0; k < size; ++k)
            {
                if (!inv::SlotAt(b.bucket, k, cur) || cur.item != item || cur.count <= 0) continue;
                if (cur.locked) { lockedSeen = true; continue; }
                if (!found || cur.instance > s.instance) { s = cur; slot = k; found = true; }
            }
            onlyLocked = !found && lockedSeen;
            return found;
        }

        void End(Job& j, Reason whenNothingMoved)
        {
            if (j.moved > 0) Finish(j.item, j.lastStorage, kStored, j.moved);
            else Finish(j.item, -1, whenNothingMoved, 0);
            j.item = 0xFFFF;
        }

        // Offers the job's item to each storage from job.nextOrder on. True when one
        // took the request; the job then waits for that storage to show it.
        bool Send(Job& j, const Bag& b, const Settings::Values& v, DWORD now)
        {
            uint32_t slot = 0;
            inv::Slot s;
            bool onlyLocked = false;
            if (!PickSlot(b, j.item, slot, s, onlyLocked))
            {
                if (onlyLocked) End(j, kLocked);
                else if (now - j.waitingSince > kWaitForBagMs) End(j, kNotInBag);
                return false;
            }
            // Only what was picked up, unless the player wants whole stacks. The debug
            // key always asks for a part, to test that a stack can be split.
            const bool part = v.autoStoreOnlyGained || j.debug;
            const int64_t amount = part && j.left < s.count ? j.left : s.count;

            for (; j.nextOrder < kOrderCount; ++j.nextOrder)
            {
                const int st = kOrder[j.nextOrder];
                if (!v.autoStoreTo[st] || WasRefused(j.item, st)) continue;
                if (static_cast<int32_t>(g_fullUntil[st] - now) > 0) { j.sawFull = true; continue; }
                const int idx = inv::IndexByName(inv::kRecordNames[st]);
                const uintptr_t target = idx >= 0 ? inv::BucketByIndex(b.holder, static_cast<uint16_t>(idx)) : 0;
                const int mi = target ? inv::MoveIndex(b.record, static_cast<uint16_t>(b.index), static_cast<uint16_t>(idx)) : -1;
                const int64_t before = target ? inv::ItemTotal(target, j.item) : -1;
                if (mi < 0 || before < 0) continue;
                int64_t send = amount;
                // The client check does not test the Collectibles Chest's one of each
                // (R5, probe session 1). Offer it exactly one, and only when the chest
                // has none; the server's own test still has the last word.
                if (st == kCollecting)
                {
                    if (before != 0) continue;
                    send = 1;
                }
                uint32_t err = inv::kErrNotWritten;
                if (!inv::Move(b.holder, &err, b.actor, j.item, s.variant, static_cast<uint64_t>(send), static_cast<uint16_t>(b.index),
                               static_cast<uint16_t>(slot), static_cast<uint32_t>(mi)))
                {
                    LOG_ERR("[deposit] the game's move function raised an exception; auto-store is off until the game restarts");
                    g_broken = true;
                    return false;
                }
                if (err == inv::kErrNone)
                {
                    j.phase = Phase::Confirm;
                    j.storage = st;
                    j.target = target;
                    j.targetBefore = before;
                    j.amount = send;
                    j.sentAt = now;
                    LOG("[deposit] item %u: sent %lld of the %lld in bag slot %u to %s, which held %lld", j.item, static_cast<long long>(send),
                        static_cast<long long>(s.count), slot, Settings::StorageLabel(st), static_cast<long long>(before));
                    return true;
                }
                if (err == inv::kErrSlotNotExist || err == inv::kErrNoMoreInsert)
                {
                    g_fullUntil[st] = now + kFullForMs;
                    j.sawFull = true;
                    LOG("[deposit] %s is full; not asked again for %u s", Settings::StorageLabel(st), kFullForMs / 1000);
                }
                else if (err != inv::kErrCantMoveItem)
                    LOG("[deposit] item %u: %s answered %s (0x%08X)", j.item, Settings::StorageLabel(st), inv::ErrName(err), err);
            }
            End(j, j.sawFull ? kStorageFull : kNoStorageTakesIt);
            return false;
        }

        // The storage's own count of the item, read again. A stack that merges into
        // one already there leaves the used slots alone, so the total is the test.
        void Confirm(Job& j, DWORD now)
        {
            const int64_t after = inv::ItemTotal(j.target, j.item);
            const int64_t arrived = after >= 0 ? after - j.targetBefore : 0;
            if (arrived >= j.amount || (arrived > 0 && now - j.sentAt > kConfirmMs))
            {
                const int64_t moved = arrived < j.amount ? arrived : j.amount;
                j.moved += moved;
                j.left -= moved;
                j.lastStorage = j.storage;
                j.phase = Phase::Find;
                j.nextOrder = 0;
                j.waitingSince = now;
                // Whole stacks move once. With only what was picked up, more may be
                // left when the pickup spread over two stacks; look for it.
                if (j.left <= 0 || !Settings::Get().autoStoreOnlyGained || j.debug) End(j, kStored);
                return;
            }
            if (now - j.sentAt <= kConfirmMs) return;
            // The client check passed but nothing arrived: the server said no.
            LOG("[deposit] item %u: %s took the request but the server kept the item in the bag", j.item, Settings::StorageLabel(j.storage));
            RememberRefused(j.item, j.storage);
            j.phase = Phase::Find;
            ++j.nextOrder;
            j.waitingSince = now;
        }

        void Compact()
        {
            int w = 0;
            for (int i = 0; i < g_jobCount; ++i)
                if (g_jobs[i].item != 0xFFFF) g_jobs[w++] = g_jobs[i];
            g_jobCount = w;
        }
    }

    bool Available() { return inv::CanMove() && !g_broken; }

    bool Queue(uint16_t item, int64_t gained)
    {
        if (item == 0xFFFF || gained <= 0 || !Settings::Get().autoStore || !Available()) return false;
        return Push(Request{item, gained, false});
    }

    void DebugNextBagStack()
    {
        static uint32_t next = 0;
        if (!Available()) { LOG_NOTE("[deposit] the move function was not found, so there is nothing to test"); return; }
        Bag b;
        if (!ReadBag(b)) { LOG_NOTE("[deposit] no character or bag right now"); return; }
        const Settings::Values& v = Settings::Get();
        const uint32_t size = inv::SlotCount(b.bucket);
        inv::Slot s;
        for (uint32_t step = 0; step < size; ++step)
        {
            const uint32_t k = (next + step) % size;
            if (!inv::SlotAt(b.bucket, k, s) || s.item == 0xFFFF || s.count <= 0 || s.locked || NeverMove(v, s.item)) continue;
            next = k + 1;
            const int64_t half = s.count > 1 ? s.count / 2 : 1;
            LOG_NOTE("[deposit] Ctrl+F11: bag slot %u, item %u x%lld; depositing %lld as if just picked up", k, s.item,
                     static_cast<long long>(s.count), static_cast<long long>(half));
            if (!Push(Request{s.item, half, true})) LOG_NOTE("[deposit] Ctrl+F11: the queue is full");
            return;
        }
        LOG_NOTE("[deposit] Ctrl+F11: nothing in the bag to deposit");
    }

    void Tick(bool canMove)
    {
        // Take what arrived since the last frame.
        Request in[kQueueMax];
        AcquireSRWLockExclusive(&g_queueLock);
        const int n = g_queueCount;
        memcpy(in, g_queue, sizeof(Request) * n);
        g_queueCount = 0;
        ReleaseSRWLockExclusive(&g_queueLock);

        const Settings::Values& v = Settings::Get();
        const DWORD now = GetTickCount();
        for (int i = 0; i < n; ++i)
        {
            const Request& r = in[i];
            if ((!v.autoStore && !r.debug) || !Available()) { Finish(r.item, -1, kOff, 0); continue; }
            if (NeverMove(v, r.item)) { Finish(r.item, -1, kNeverMoved, 0); continue; }
            // A second pickup of an item still waiting for its turn adds to that job.
            bool merged = false;
            for (int k = 0; k < g_jobCount && !merged; ++k)
            {
                Job& o = g_jobs[k];
                if (o.item == r.item && o.phase == Phase::Find && o.debug == r.debug) { o.left += r.gained; merged = true; }
            }
            if (merged) continue;
            if (g_jobCount >= kJobMax) { Finish(r.item, -1, kBusy, 0); continue; }
            Job& j = g_jobs[g_jobCount++];
            j = Job{};
            j.item = r.item;
            j.left = r.gained;
            j.debug = r.debug;
            j.waitingSince = now;
        }
        if (!g_jobCount) return;

        Bag b;
        const bool bag = ReadBag(b);
        int sends = 0;
        for (int i = 0; i < g_jobCount; ++i)
        {
            Job& j = g_jobs[i];
            if (j.item == 0xFFFF) continue;
            if (g_broken) { End(j, kOff); continue; }
            if (j.phase == Phase::Confirm)
            {
                // A load screen or a menu does not count against the wait.
                if (!bag) { j.sentAt = now; continue; }
                Confirm(j, now);
                continue;
            }
            // Nothing is sent outside free play or with a storage open; the wait for
            // the bag starts over when play resumes.
            if (!bag || !canMove) { j.waitingSince = now; continue; }
            if (sends >= kSendsPerFrame || ItemInFlight(j.item, &j)) continue;
            if (Send(j, b, v, now)) ++sends;
        }
        Compact();
    }

    int TakeResults(Result* out, int max)
    {
        if (!out || max <= 0) return 0;
        AcquireSRWLockExclusive(&g_resultLock);
        int n = 0;
        while (n < max && g_resultCount > 0)
        {
            out[n++] = g_results[g_resultHead];
            g_resultHead = (g_resultHead + 1) % kResultMax;
            --g_resultCount;
        }
        ReleaseSRWLockExclusive(&g_resultLock);
        return n;
    }
}

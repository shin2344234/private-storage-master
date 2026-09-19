#include "storage/storage.h"

#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "core/settings.h"
#include "game/addresses.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "storage/capacity.h"
#include "storage/deposit.h"
#include "storage/inventory.h"
#include "storage/pad.h"

namespace psm::storage
{
    namespace
    {
        addr::Storage A;
        std::atomic<bool> g_stop{false};
        HANDLE g_poller = nullptr;

        // ------------------------------------------------------------ game structures
        // staticstringA node (R3A 2). A negative refcount makes the game skip every
        // addref and release, so these nodes are never freed by it, and the mod
        // keeps them for the life of the process.
        struct SsNode
        {
            const char* p;
            uint32_t len;
            uint32_t hash;     // 0xFFFFFFFF until the game caches one
            int32_t  ref;
            uint8_t  flag;
            uint8_t  pad[3];
            char     text[1];  // allocated past the end
        };
        static_assert(offsetof(SsNode, len) == 0x08 && offsetof(SsNode, ref) == 0x10 &&
                      offsetof(SsNode, flag) == 0x14 && offsetof(SsNode, text) == 0x18, "staticstringA node layout");

        SsNode* MakeSs(const char* s)
        {
            const size_t n = strlen(s);
            auto* node = static_cast<SsNode*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SsNode) + n + 1));
            if (!node) return nullptr;
            node->len = static_cast<uint32_t>(n);
            node->hash = 0xFFFFFFFF;
            node->ref = -1;
            memcpy(node->text, s, n + 1);
            node->p = node->text;
            return node;
        }

        struct Arg  { uint8_t type; uint8_t pad[7]; uint64_t value; };
        struct Sub  { uint8_t kind; uint8_t pad[7]; Arg* args; uint32_t count; uint32_t cap; };
        struct Data { uint8_t kind; uint8_t pad[7]; Sub* subs; uint32_t count; uint32_t cap; };
        static_assert(sizeof(Arg) == 0x10 && sizeof(Sub) == 0x18 && offsetof(Data, subs) == 8 && offsetof(Data, count) == 0x10, "packet layout");

        using PostFn  = void(__fastcall*)(uintptr_t wrap, uint32_t actor, uintptr_t* view, uintptr_t* selector, uint64_t* stageId, Data* data);
        using PhaseFn = uintptr_t(__fastcall*)(uintptr_t pm, uint32_t phase, uint32_t on);
        using InputBlockFn = void(__fastcall*)(uintptr_t stageMgr, uint64_t* key, uint32_t mode);
        using Fn4 = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

        constexpr uint32_t kPlayerActor     = 0xA0100001;  // what every natural open carried
        constexpr uint8_t  kPhaseIngameMenu = 0x0E;
        // Verified unchanged between 1.0.0.2850 and 1.0.0.2944: the screen name
        // table is compared in the same order on both, with Ingame at index 16.
        // The byte holding this value moved, not the value, so the offset is
        // derived (A.phaseScreenOff) while the constant stays written down.
        constexpr uint8_t  kScreenIngame    = 0x10;

        struct Chest
        {
            const char* label;
            const char* setInventory;
            const char* title;
            const char* modalHash;   // sub 0x07 of the 0x0D packet; nullptr: the chart sends no 0x0D
            const char* titleHash;   // sub 0x08 of the 0x14 header: lookup3 of the lowercased title key
            const char* icon;        // sub 0x0B of the 0x14 header; nullptr: the chart leaves it empty
            const char* extra;       // another 0x15 command sent before SetInventory, or nullptr
        };
        // Order matches Settings. Commands come from the game's stage charts.
        const Chest kChests[Settings::kStorages] = {
            { "Private Storage", "SetInventory(Character,Focus,True,Default;CampWareHouse,Focus,True,Default)",
              "SetWareHouseInventoryName(UI_WareHouse_CampStroage)", "197270237", "1494912655", "cd_icon_map_bank", nullptr },
            { "Gatherables Chest", "SetInventory(Character,Focus,True;Housing_GatheredMaterials,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_HousingGatheredMaterials)", "2520550823", "1329171849", "cd_icon_map_bank", nullptr },
            { "Wardrobe", "SetInventory(Character,Focus,True;Housing_Dresser,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_HousingFurnitureDresser)", "2520550823", "1469492791", "cd_icon_map_bank", nullptr },
            { "Kuku Cooler", "SetInventory(Character,Focus,True;Housing_Refrigerator,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_HousingRefrigerator)", "2520550823", "295797541", "cd_icon_map_bank", nullptr },
            { "Collectibles Chest", "SetInventory(Character,Focus,True;Housing_Collecting,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_HousingCollecting)", "2520550823", "52739647", "cd_icon_map_bank", nullptr },
            { "Camp Straw", "SetInventory(Character,Focus,True;CampStraw,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_CampStraw)", "2520550823", "502364378", "cd_icon_map_bank", nullptr },
            { "Bird Feed", "SetInventory(Character,Focus,True;BirdFeed,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_BirdFeed)", "2933432374", "3229017175", "cd_icon_map_bank", nullptr },
            // The town warehouse NPC's WareHouse variant, without SetDonationFaction
            // (it needs the NPC actor) and without the 0x09 NPC packet.
            { "Town Warehouse",
              "SetInventory(Character,Focus,True,Default;WareHouse,Focus,True;Wagon,NearWagon,False;PetAndVehicle,NearMercenary_Vehicle,False;Ship,Ship,False)",
              "SetWareHouseInventoryName(UI_WareHouse_Stroage)", nullptr, "2638607143", nullptr, "ShowPackageCampMoneyList()" },
            // The Kuku inventory, where Abyss gears are kept. The game shows it at the
            // Kuku pot NPC on its own panel; it opens fine in the warehouse screen.
            { "Abyss gear storage", "SetInventory(Character,Focus,True;Kuku,Focus,True)",
              "SetWareHouseInventoryName(UI_Inventory_KukuItemList)", "2520550823", "3472366085", "cd_icon_map_kukushop", nullptr },
        };

        // Stage ids for the mod's screens. Live ids are small and count up from boot.
        uint64_t g_nextId = 900000000;
        SsNode* g_view = nullptr, *g_selector = nullptr;
        SsNode* g_itemText[Settings::kStorages][8] = {};   // per chest: icon, title hash, modal hash, extra, setInventory, title

        // ------------------------------------------------------------ shared state
        // Requests from the keyboard and pad, run on the main thread by the mode switch.
        enum : int { kActNone = -1 };
        std::atomic<int> g_pending{kActNone};

        std::atomic<uintptr_t> g_warehouse{0};   // UIGamePlayControlRootWarehouse2 instance
        std::atomic<uintptr_t> g_stageMgr{0};    // player's ClientSequencerStageManager

        // Main thread. The atomics are also read by the poller and the hooks.
        std::atomic<bool>     g_open{false};
        std::atomic<uint64_t> g_openId{0};
        std::atomic<int>      g_openChest{-1};
        std::atomic<bool>     g_saw0E{false};
        std::atomic<bool>     g_closeRequested{false};
        uint32_t g_frames = 0;
        uintptr_t g_blockMgr = 0;
        uint64_t  g_blockKey = 0;
        int      g_switchTo = -1;       // open this one once the last close has gone through
        uint32_t g_switchFrames = 0;
        std::atomic<DWORD> g_closedAt{0};
        std::atomic<DWORD> g_openedAt{0};
        constexpr DWORD kReopenCooldownMs = 250;
        std::atomic<DWORD> g_pauseUntil{0};
        std::atomic<bool> g_ready{false};
        // Published by Tick for the capacity worker, which runs on its own thread.
        std::atomic<bool>  g_freePlay{false};
        std::atomic<DWORD> g_lastTick{0};
        std::atomic<bool>  g_hidingKeys{false};   // published by the poller for PsmHidingKeys
        bool Paused() { return static_cast<int32_t>(g_pauseUntil.load() - GetTickCount()) > 0; }

        void* oHandler = nullptr, *oModeSwitch = nullptr, *oStageClose = nullptr, *oInputBlock = nullptr;

        // ------------------------------------------------------------ posting
        uintptr_t EventWrap()
        {
            uintptr_t mgr = 0, wrap = 0, vt = 0;
            if (!mem::ReadPtr(A.eventManagerGlobal, &mgr) || !mem::ReadPtr(mgr + A.eventWrapOff, &wrap) || !mem::ReadPtr(wrap, &vt)) return 0;
            return vt == A.eventWrapVtable ? wrap : 0;
        }

        bool PostGuarded(uintptr_t wrap, uintptr_t* view, uintptr_t* sel, uint64_t* id, Data* d)
        {
            __try { static_cast<PostFn>(reinterpret_cast<void*>(A.eventPost))(wrap, kPlayerActor, view, sel, id, d); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool PhaseGuarded(uintptr_t pm, uint8_t phase, bool on)
        {
            __try { static_cast<PhaseFn>(reinterpret_cast<void*>(A.requestPhase))(pm, phase, on ? 1 : 0); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool InputBlockGuarded(uintptr_t mgr, uint64_t key, uint32_t mode)
        {
            __try { static_cast<InputBlockFn>(reinterpret_cast<void*>(A.inputBlockSet))(mgr, &key, mode); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        bool Post(uint8_t kind, uint64_t stageId, const SsNode* idText, Sub* extra, uint32_t extraCount)
        {
            const uintptr_t wrap = EventWrap();
            if (!wrap) { LOG_ERR("[storage] the stage chart event wrap is gone"); return false; }
            Arg idArg{}; idArg.type = 5; idArg.value = reinterpret_cast<uint64_t>(idText);
            Sub subs[4]{};
            subs[0].kind = 0x00; subs[0].args = &idArg; subs[0].count = subs[0].cap = 1;
            if (extraCount > 3) extraCount = 3;
            for (uint32_t i = 0; i < extraCount; ++i) subs[1 + i] = extra[i];
            Data d{};
            d.kind = kind; d.subs = subs; d.count = d.cap = 1 + extraCount;
            uintptr_t view = reinterpret_cast<uintptr_t>(g_view), sel = reinterpret_cast<uintptr_t>(g_selector);
            uint64_t id = stageId;
            const bool ok = PostGuarded(wrap, &view, &sel, &id, &d);
            if (!ok) LOG_ERR("[storage] posting event 0x%02X for stage %llu faulted", kind, stageId);
            else LOG("[storage] posted 0x%02X for stage %llu", kind, stageId);
            return ok;
        }

        // The player's ClientSequencerStageManager. The game's own InputBlock calls
        // name it; failing that, look in the user actor and controlled character.
        uintptr_t StageManager()
        {
            uintptr_t known = g_stageMgr.load(), v = 0;
            if (known && mem::ReadPtr(known, &v) && v == A.stageMgrVtable) return known;
            uintptr_t g = 0, mgr = 0, user = 0, ch = 0;
            if (!mem::ReadPtr(A.actorManagerGlobal, &g) || !mem::ReadPtr(g + 0x30, &mgr) || !mem::ReadPtr(mgr + 0x58, &user)) return 0;
            mem::ReadPtr(user + 0xD8, &ch);
            const uintptr_t owners[2] = {user, ch};
            for (uintptr_t owner : owners)
            {
                if (!owner) continue;
                uintptr_t comps = 0;
                const uintptr_t tables[2] = {owner, mem::ReadPtr(owner + 0x68, &comps) ? comps : 0};
                for (uintptr_t table : tables)
                {
                    if (!table) continue;
                    for (unsigned off = 0; off < 0x800; off += 8)
                    {
                        uintptr_t q = 0;
                        if (!mem::ReadPtr(table + off, &q) || !mem::ReadPtr(q, &v) || v != A.stageMgrVtable) continue;
                        g_stageMgr = q;
                        return q;
                    }
                }
            }
            return 0;
        }

        bool GateOpen(uintptr_t pm, char* why, size_t cap)
        {
            uint8_t mode = 0, screen = 0, blocked = 0;
            uint32_t queued = 0;
            uintptr_t proc = 0;
            mem::Read8(pm + 0x28, &mode);
            mem::Read8(pm + A.phaseScreenOff, &screen);
            mem::Read32(pm + 0x70, &queued);
            if (mem::ReadPtr(pm + 0x10, &proc)) mem::Read8(proc + 0xA0, &blocked);
            snprintf(why, cap, "mode %u screen 0x%02X queued %u blocked %u", mode, screen, queued, blocked);
            return mode == 4 && screen == kScreenIngame && queued == 0 && proc && blocked == 0;
        }

        void Forget()
        {
            g_blockMgr = 0;
            g_blockKey = 0;
            g_open = false;
            g_openChest = -1;
            g_closedAt = GetTickCount();
        }

        void Close(uintptr_t pm, const char* reason)
        {
            const uint64_t id = g_openId.load();
            const int chest = g_openChest.load();
            LOG("[storage] close %s (stage %llu): %s", chest >= 0 ? kChests[chest].label : "?", id, reason);
            char text[32];
            snprintf(text, sizeof text, "%llu", id);
            Post(0x0F, id, MakeSs(text), nullptr, 0);
            if (!PhaseGuarded(pm, kPhaseIngameMenu, false)) LOG_ERR("[storage] leaving the menu phase faulted");
            if (g_blockMgr && !InputBlockGuarded(g_blockMgr, g_blockKey, 0)) LOG_ERR("[storage] removing the input block faulted");
            if (pad::Last() & 0x2000) pad::HideBUntilReleased();   // B held: keep it from the game so it does not dodge
            Forget();
        }

        bool Open(uintptr_t pm, int chest)
        {
            const Chest& c = kChests[chest];
            char why[128];
            if (!GateOpen(pm, why, sizeof why)) { LOG_NOTE("[storage] %s not opened, the game is not in free play (%s)", c.label, why); return false; }
            if (!EventWrap()) { LOG_ERR("[storage] %s not opened: the stage chart event wrap was not found", c.label); return false; }
            const uintptr_t mgr = StageManager();
            if (!mgr) { LOG_ERR("[storage] %s not opened: the player's stage manager was not found", c.label); return false; }

            const uint64_t id = ++g_nextId;
            char idText[32];
            snprintf(idText, sizeof idText, "%llu", id);
            SsNode* idNode = MakeSs(idText);
            SsNode** t = g_itemText[chest];
            if (!idNode || !t[4]) { LOG_ERR("[storage] out of memory"); return false; }
            LOG("[storage] open %s, stage %llu", c.label, id);

            g_openId = id;
            g_saw0E = false;
            g_openChest = chest;
            g_frames = 0;
            g_openedAt = GetTickCount();
            g_open = true;

            // Chart order: input block, phase, then the UIControl events.
            if (InputBlockGuarded(mgr, id, 2)) { g_blockMgr = mgr; g_blockKey = id; }
            else LOG_ERR("[storage] the input block faulted");
            if (!PhaseGuarded(pm, kPhaseIngameMenu, true)) LOG_ERR("[storage] the menu phase request faulted");

            Post(0x12, id, idNode, nullptr, 0);
            Arg iconArg{};  iconArg.type = 1;  iconArg.value = reinterpret_cast<uint64_t>(t[0]);
            Arg titleArg{}; titleArg.type = 9; titleArg.value = reinterpret_cast<uint64_t>(t[1]);
            Sub header[2]{};
            header[0].kind = 0x0B; header[0].args = c.icon ? &iconArg : nullptr; header[0].count = header[0].cap = c.icon ? 1 : 0;
            header[1].kind = 0x08; header[1].args = &titleArg; header[1].count = header[1].cap = 1;
            Post(0x14, id, idNode, header, 2);
            if (c.modalHash)
            {
                Arg hashArg{}; hashArg.type = 9; hashArg.value = reinterpret_cast<uint64_t>(t[2]);
                Sub s7{}; s7.kind = 0x07; s7.args = &hashArg; s7.count = s7.cap = 1;
                Post(0x0D, id, idNode, &s7, 1);
            }
            Arg extraArg{}; extraArg.type = 5; extraArg.value = reinterpret_cast<uint64_t>(t[3]);
            Arg invArg{};   invArg.type = 5;   invArg.value = reinterpret_cast<uint64_t>(t[4]);
            Arg nameArg{};  nameArg.type = 5;  nameArg.value = reinterpret_cast<uint64_t>(t[5]);
            Sub cmds[3]{};
            uint32_t n = 0;
            if (c.extra) { cmds[n].kind = 0x0E; cmds[n].args = &extraArg; cmds[n].count = cmds[n].cap = 1; ++n; }
            cmds[n].kind = 0x0E; cmds[n].args = &invArg;  cmds[n].count = cmds[n].cap = 1; ++n;
            cmds[n].kind = 0x0E; cmds[n].args = &nameArg; cmds[n].count = cmds[n].cap = 1; ++n;
            Post(0x15, id, idNode, cmds, n);
            Post(0x0E, id, idNode, nullptr, 0);
            return true;
        }

        // A key or combo for storage `chest`.
        void Request(uintptr_t pm, int chest)
        {
            if (g_open.load())
            {
                const int cur = g_openChest.load();
                if (cur == chest) { Close(pm, "its key again"); return; }
                LOG("[storage] switching from %s to %s", cur >= 0 ? kChests[cur].label : "?", kChests[chest].label);
                Close(pm, "switching");
                g_switchTo = chest;
                g_switchFrames = 0;
                return;
            }
            if (GetTickCount() - g_closedAt.load() < kReopenCooldownMs) { LOG("[storage] %s ignored, a storage closed just now", kChests[chest].label); return; }
            Open(pm, chest);
        }

        // Once per frame, after the game's own mode switch.
        void Tick(uintptr_t pm)
        {
            const int act = g_pending.exchange(kActNone);
            if (act >= 0 && act < Settings::kStorages) { g_switchTo = -1; Request(pm, act); }

            if (g_closeRequested.exchange(false) && g_open.load()) Close(pm, "Esc or B");

            uint8_t mode = 0, screen = 0;
            mem::Read8(pm + 0x28, &mode);
            mem::Read8(pm + A.phaseScreenOff, &screen);
            g_freePlay = mode == 4 && screen == kScreenIngame;
            g_lastTick = GetTickCount();
            // Loot moves only in free play, with no storage open or about to open,
            // so a deposit never races the warehouse screen.
            inv::RefreshPlayerBag();
            deposit::Tick(g_freePlay.load() && !g_open.load() && g_switchTo < 0);

            if (g_switchTo >= 0 && !g_open.load())
            {
                char why[128];
                if (GateOpen(pm, why, sizeof why)) { const int to = g_switchTo; g_switchTo = -1; Open(pm, to); }
                else if (++g_switchFrames > 120) { LOG("[storage] gave up switching to %s: %s", kChests[g_switchTo].label, why); g_switchTo = -1; }
            }

            if (!g_open.load()) return;
            ++g_frames;
            // Quitting to the title with a screen open tears down the stage manager
            // and the UI (mode 5, then 7). Forget the screen without touching either.
            if (mode != 4)
            {
                LOG("[storage] the game left play (mode %u) with %s open; forgetting it", mode, kChests[g_openChest.load()].label);
                Forget();
                return;
            }
            if (g_frames == 150)
            {
                if (!g_saw0E.load()) { Close(pm, "the warehouse screen never received the open"); return; }
                if (screen != kPhaseIngameMenu) { Close(pm, "the menu phase never started"); return; }
            }
            if (g_frames > 150)
            {
                uint64_t cur = 0;
                const uintptr_t c = g_warehouse.load();
                if (c && mem::Read64(c + 0x128, &cur) && cur != g_openId.load())
                {
                    LOG("[storage] the warehouse screen now belongs to stage %llu, not ours; letting it go", cur);
                    if (g_blockMgr) InputBlockGuarded(g_blockMgr, g_blockKey, 0);
                    Forget();
                }
                else if (screen == kScreenIngame)
                {
                    LOG("[storage] back in play without the mod's close running; letting the screen go");
                    if (g_blockMgr) InputBlockGuarded(g_blockMgr, g_blockKey, 0);
                    Forget();
                }
            }
        }

        // ------------------------------------------------------------ hooks
        // First type-5 argument of the first kind-0 sub-command, as a number.
        bool PacketStageId(uintptr_t pkt, uint64_t* out)
        {
            uintptr_t subs = 0;
            uint32_t n = 0;
            if (!mem::ReadPtr(pkt + 8, &subs) || !mem::Read32(pkt + 0x10, &n) || n > 64) return false;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uintptr_t s = subs + 0x18ull * i;
                uint8_t kind = 0;
                uintptr_t args = 0, node = 0, text = 0;
                uint32_t an = 0;
                if (!mem::Read8(s, &kind) || kind != 0) continue;
                if (!mem::ReadPtr(s + 8, &args) || !mem::Read32(s + 0x10, &an) || !an) return false;
                char t[32];
                if (!mem::ReadPtr(args + 8, &node) || !mem::ReadPtr(node, &text) || !mem::ReadCString(text, t, sizeof t)) return false;
                char* end = nullptr;
                *out = strtoull(t, &end, 10);
                return end && end != t && *end == 0;
            }
            return false;
        }

        // Warehouse2 command handler: remembers the controller, and notes when our
        // show packet arrives.
        uintptr_t __fastcall hkHandler(uintptr_t self, uintptr_t rdx, uintptr_t r8, uintptr_t pkt)
        {
            uintptr_t vt = 0;
            if (mem::ReadPtr(self, &vt) && vt == A.warehouseVtable) g_warehouse = self;
            if (g_open.load() && pkt)
            {
                uint8_t kind = 0;
                uint64_t id = 0;
                if (mem::Read8(pkt, &kind) && kind == 0x0E && PacketStageId(pkt, &id) && id == g_openId.load()) g_saw0E = true;
            }
            return static_cast<Fn4>(oHandler)(self, rdx, r8, pkt);
        }

        uintptr_t __fastcall hkModeSwitch(uintptr_t pm, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
        {
            const uintptr_t r = static_cast<Fn4>(oModeSwitch)(pm, rdx, r8, r9);
            Tick(pm);
            return r;
        }

        // "Close" to a stage. Esc and B in the warehouse call it with &controller+0x128.
        uintptr_t __fastcall hkStageClose(uintptr_t rcx, uintptr_t idPtr, uintptr_t root, uintptr_t name)
        {
            uint64_t id = 0;
            char n[8] = {};
            if (g_open.load() && mem::Read64(idPtr, &id) && id == g_openId.load() && mem::ReadCString(name, n, sizeof n) && strcmp(n, "Close") == 0)
            {
                g_closeRequested = true;
                return 0;
            }
            return static_cast<Fn4>(oStageClose)(rcx, idPtr, root, name);
        }

        uintptr_t __fastcall hkInputBlock(uintptr_t mgr, uintptr_t keyPtr, uintptr_t mode, uintptr_t r9)
        {
            uintptr_t vt = 0;
            if (mem::ReadPtr(mgr, &vt) && vt == A.stageMgrVtable && g_stageMgr.load() != mgr) g_stageMgr = mgr;
            return static_cast<Fn4>(oInputBlock)(mgr, keyPtr, mode, r9);
        }

        // ------------------------------------------------------------ keyboard
        // The game reads the keyboard from window messages (R3E 5.1), so bound keys
        // are taken out there. Modifiers are read the same way the game would.
        //
        // The game has two top-level windows of its own, "Root" and
        // "WindowsLauncherClassName", and shows one of them: Root in the probe
        // sessions of 16 September, the launcher class on 18 September with Root
        // hidden at 0x0. Both are taken, each with its own original procedure, so
        // the one that is showing is covered even if the game swaps them.
        //
        // The poller fills an entry and only then publishes it through the count;
        // the window procedure runs on the game's thread and reads entries below
        // the count. An entry's original procedure is stored before our procedure
        // goes in, so no message can arrive with nothing to pass it on to.
        struct HookedWindow { HWND hwnd; WNDPROC old; bool unicode; };
        constexpr int kMaxWindows = 4;
        HookedWindow g_windows[kMaxWindows] = {};
        std::atomic<int> g_windowCount{0};
        bool    g_swallowed[256] = {};
        bool    g_eatChar = false;

        uint8_t HeldMods()
        {
            uint8_t m = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) m |= Settings::kModCtrl;
            if (GetKeyState(VK_SHIFT) & 0x8000) m |= Settings::kModShift;
            if (GetKeyState(VK_MENU) & 0x8000) m |= Settings::kModAlt;
            return m;
        }

        // -2: the dump key; -1: not bound; else a storage.
        int Binding(uint8_t vk, uint8_t mods)
        {
            const Settings::Values& v = Settings::Get();
            for (int i = 0; i < Settings::kStorages; ++i)
                if (v.key[i].vk == vk && v.key[i].mods == mods) return i;
            if (v.dumpKey.vk == vk && v.dumpKey.mods == mods) return -2;
            if (v.hideKeysToggleKey.vk == vk && v.hideKeysToggleKey.mods == mods) return -3;
            return -1;
        }

        // A modifier combination some storage key (or the dump key) is bound to.
        bool ModsInUse(uint8_t mods)
        {
            if (!mods) return false;
            const Settings::Values& v = Settings::Get();
            if (v.dumpKey.vk && v.dumpKey.mods == mods) return true;
            if (v.hideKeysToggleKey.vk && v.hideKeysToggleKey.mods == mods) return true;
            for (int i = 0; i < Settings::kStorages; ++i)
                if (v.key[i].vk && v.key[i].mods == mods) return true;
            return false;
        }

        // Keys held back from the game while a storage modifier is down: the
        // function keys, the row every default storage key sits on, so a slip
        // from Ctrl+F1 to F2 or F3 cannot fire the game's own F key. Nothing else.
        // Ctrl is the game's guard and Examine, and players press every other key
        // with it held: Ctrl+F kicks while guarding (Key_KickAttack, plain F), and
        // while Examine is up the game reads Q, E, R, T and G as plain keys to
        // talk, trade and give gifts (GimmickInput, InteractionX to B). 1.0.1 and
        // 1.1.0 held back everything but a pass list and kept losing those. Alt+F4
        // always goes through.
        bool HoldBackWithModifier(uint8_t vk, uint8_t mods)
        {
            if (vk < VK_F1 || vk > VK_F12) return false;
            return !(vk == VK_F4 && (mods & Settings::kModAlt));
        }

        // Hands a message to the procedure that window had before ours.
        LRESULT PassOn(HWND h, UINT m, WPARAM w, LPARAM l)
        {
            const int n = g_windowCount.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i)
            {
                const HookedWindow& e = g_windows[i];
                if (e.hwnd == h) return e.unicode ? CallWindowProcW(e.old, h, m, w, l) : CallWindowProcA(e.old, h, m, w, l);
            }
            return DefWindowProcW(h, m, w, l);   // not a window we took; never reached, but never call through nothing
        }

        LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
        {
            if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !Paused())
            {
                const uint8_t vk = static_cast<uint8_t>(w & 0xFF);
                const uint8_t mods = HeldMods();
                const int b = Binding(vk, mods);
                g_eatChar = false;
                const bool lockout = b == -1 && Settings::Get().hideKeysWithModifier && ModsInUse(mods) && HoldBackWithModifier(vk, mods);
                if (b != -1 || lockout)
                {
                    // Only hidden here. The poller acts on it, reading the keyboard the way
                    // probe 2 did, so a message that never reaches this window still works.
                    g_swallowed[vk] = true;
                    g_eatChar = true;
                    return 0;
                }
            }
            else if (m == WM_KEYUP || m == WM_SYSKEYUP)
            {
                const uint8_t vk = static_cast<uint8_t>(w & 0xFF);
                if (g_swallowed[vk]) { g_swallowed[vk] = false; return 0; }
            }
            else if ((m == WM_CHAR || m == WM_SYSCHAR) && g_eatChar)
            {
                g_eatChar = false;
                return 0;
            }
            return PassOn(h, m, w, l);
        }

        // The game's own window classes. Anything else in the process, the IME
        // window or another mod's, is left alone.
        bool GameWindowClass(HWND h)
        {
            char cls[64] = {};
            GetClassNameA(h, cls, sizeof cls);
            return strcmp(cls, "Root") == 0 || strcmp(cls, "WindowsLauncherClassName") == 0;
        }

        struct Found { HWND h[kMaxWindows]; int n; };

        BOOL CALLBACK FindGameWindows(HWND h, LPARAM out)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId() || GetWindow(h, GW_OWNER) || !GameWindowClass(h)) return TRUE;
            auto* f = reinterpret_cast<Found*>(out);
            if (f->n < kMaxWindows) f->h[f->n++] = h;
            return TRUE;
        }

        // Takes every game window not taken yet, and returns how many of the taken
        // ones are showing. A window already in the table is never taken twice,
        // even when its procedure is no longer ours: Master Looter's overlay
        // subclasses after us, and taking the window again would store its
        // procedure as the original and send the two in a loop.
        int SubclassGameWindows()
        {
            Found f{};
            EnumWindows(FindGameWindows, reinterpret_cast<LPARAM>(&f));
            for (int k = 0; k < f.n; ++k)
            {
                const HWND h = f.h[k];
                const int n = g_windowCount.load(std::memory_order_relaxed);
                bool known = false;
                for (int i = 0; i < n; ++i) known |= g_windows[i].hwnd == h;
                if (known || n >= kMaxWindows) continue;

                const bool unicode = IsWindowUnicode(h) != 0;
                const LONG_PTR cur = unicode ? GetWindowLongPtrW(h, GWLP_WNDPROC) : GetWindowLongPtrA(h, GWLP_WNDPROC);
                if (!cur) continue;
                g_windows[n] = HookedWindow{h, reinterpret_cast<WNDPROC>(cur), unicode};
                g_windowCount.store(n + 1, std::memory_order_release);
                const LONG_PTR prev = unicode ? SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc))
                                              : SetWindowLongPtrA(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc));
                char cls[64] = {};
                GetClassNameA(h, cls, sizeof cls);
                if (!prev)
                {
                    LOG_ERR("[keys] could not take the keys of game window %s (%lu)", cls, GetLastError());
                    continue;
                }
                // Something swapped the procedure between the read and ours going in.
                if (prev != cur) g_windows[n].old = reinterpret_cast<WNDPROC>(prev);
                LOG("[keys] hiding bound keys from game window %p, class %s%s", static_cast<void*>(h), cls,
                    IsWindowVisible(h) ? "" : ", hidden for now");
            }
            int showing = 0;
            const int n = g_windowCount.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i)
                if (IsWindow(g_windows[i].hwnd) && IsWindowVisible(g_windows[i].hwnd)) ++showing;
            return showing;
        }

        void RestoreGameWindows()
        {
            const int n = g_windowCount.load(std::memory_order_acquire);
            for (int i = 0; i < n; ++i)
            {
                const HookedWindow& e = g_windows[i];
                if (!IsWindow(e.hwnd)) continue;
                const LONG_PTR cur = e.unicode ? GetWindowLongPtrW(e.hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(e.hwnd, GWLP_WNDPROC);
                if (cur != reinterpret_cast<LONG_PTR>(WndProc)) continue;   // someone chained after us; leave it
                if (e.unicode) SetWindowLongPtrW(e.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(e.old));
                else SetWindowLongPtrA(e.hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(e.old));
            }
        }

        // ------------------------------------------------------------ controller
        bool GameInFront()
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            return pid == GetCurrentProcessId();
        }

        constexpr DWORD kHoldCloseMs = 700;


        bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

        // Flips HideKeysWithModifier and saves it, the same way Master Looter's
        // Storage tab changes a setting. Always logged, since it is something the
        // player did on purpose and the next log should say which way it went.
        void ToggleHideKeys()
        {
            bool on = false;
            char why[160];
            const bool saved = Settings::Update([&on](Settings::Values& v) { on = v.hideKeysWithModifier = !v.hideKeysWithModifier; }, why, sizeof why);
            LOG_NOTE("[keys] HideKeysWithModifier is now %s%s%s", on ? "on" : "off",
                     saved ? "" : ", but it was not saved: ", saved ? "" : why);
        }

        DWORD WINAPI Poller(LPVOID)
        {
            bool was[Settings::kStorages] = {};
            bool keyWas[Settings::kStorages + 2] = {};   // the storages, then the dump key, then the block toggle
            DWORD since[Settings::kStorages] = {};
            bool held[Settings::kStorages] = {};
            DWORD nextWindowCheck = 0;
            // 1.0.0 looked for one window class, found nothing on either game
            // build and said nothing, for 24 sessions. Missing it is worth an
            // error line, once, and finding it later is worth a note, once.
            const DWORD pollStart = GetTickCount();
            bool saidMissing = false, saidFound = false;
            while (!g_stop.load())
            {
                Sleep(16);
                const DWORD now = GetTickCount();
                if (now >= nextWindowCheck)
                {
                    const int showing = SubclassGameWindows();
                    if (!showing && !saidMissing && now - pollStart > 60000)
                    {
                        LOG_ERR("[keys] no game window to take keys from after a minute (looked for Root and WindowsLauncherClassName). "
                                "Storage keys still work, but bound keys also reach the game and HideKeysWithModifier does nothing.");
                        saidMissing = true;
                    }
                    else if (showing && saidMissing && !saidFound)
                    {
                        LOG_NOTE("[keys] found the game window after all; bound keys are hidden from the game now");
                        saidFound = true;
                    }
                    nextWindowCheck = now + 500;
                }

                const bool front = GameInFront();
                const bool paused = Paused();
                const uint16_t buttons = front ? pad::Poll() : 0;
                const Settings::Values& v = Settings::Get();

                uint8_t mods = 0;
                if (front)
                {
                    if (KeyDown(VK_CONTROL)) mods |= Settings::kModCtrl;
                    if (KeyDown(VK_SHIFT)) mods |= Settings::kModShift;
                    if (KeyDown(VK_MENU)) mods |= Settings::kModAlt;
                }
                g_hidingKeys.store(v.hideKeysWithModifier && ModsInUse(mods), std::memory_order_relaxed);
                for (int i = 0; i <= Settings::kStorages + 1; ++i)
                {
                    const Settings::KeyBind& k = i < Settings::kStorages ? v.key[i] : i == Settings::kStorages ? v.dumpKey : v.hideKeysToggleKey;
                    const bool down = front && k.vk && k.mods == mods && KeyDown(k.vk);
                    if (down && !keyWas[i] && !paused)
                    {
                        char t[32];
                        LOG("[keys] %s", Settings::KeyText(k, t, sizeof t));
                        if (i < Settings::kStorages) g_pending = i;
                        else if (i == Settings::kStorages) capacity::RequestDump();
                        else ToggleHideKeys();
                    }
                    keyWas[i] = down;
                }
#ifdef PSM_DEPOSIT_TEST_KEY
                {
                    // Deposit test (auto-store): Ctrl+F11 with DebugLog=1, in test builds only.
                    // A release leaves it out, because players set DebugLog=1 to send a log.
                    static bool probeWas = false;
                    const bool down = front && v.debugLog && mods == Settings::kModCtrl && KeyDown(VK_F11);
                    if (down && !probeWas && !paused) deposit::DebugNextBagStack();
                    probeWas = down;
                }
#endif
                for (int i = 0; i < Settings::kStorages; ++i)
                {
                    const Settings::PadBind& p = v.pad[i];
                    const bool down = p.press && (buttons & p.press) && (buttons & p.hold) == p.hold;
                    if (down && !was[i])
                    {
                        since[i] = now;
                        held[i] = paused;   // a combo that began while paused never acts
                        if (!paused) g_pending = i;
                    }
                    // Holding a combo for most of a second closes a storage that was
                    // already open when the hold began, in case the press itself was lost.
                    if (down && !held[i] && now - since[i] >= kHoldCloseMs)
                    {
                        held[i] = true;
                        const int cur = g_openChest.load();
                        if (g_open.load() && cur >= 0 && static_cast<int32_t>(since[i] - g_openedAt.load()) > 0)
                        {
                            LOG("[pad] combo held, closing %s", kChests[cur].label);
                            g_pending = cur;
                        }
                    }
                    was[i] = down;
                }
            }
            g_hidingKeys = false;
            return 0;
        }

        bool Hook(const char* name, uintptr_t target, void* detour, void** orig)
        {
            char why[128];
            if (!farhook::Install(name, target, detour, orig, why, sizeof why))
            {
                LOG_ERR("[storage] hooking %s failed: %s", name, why);
                return false;
            }
            LOG_OK("[storage] hooked %s", name);
            return true;
        }
    }

    bool Start()
    {
        if (!addr::ResolveStorage(A))
        {
            LOG_ERR("[storage] this game version does not match what the mod needs, so storage keys are turned off");
            return false;
        }
        g_view = MakeSs("WareHouseView");
        g_selector = MakeSs("selector-stagechart-self");
        for (int i = 0; i < Settings::kStorages; ++i)
        {
            const Chest& c = kChests[i];
            g_itemText[i][0] = MakeSs(c.icon ? c.icon : "");
            g_itemText[i][1] = MakeSs(c.titleHash);
            g_itemText[i][2] = MakeSs(c.modalHash ? c.modalHash : "");
            g_itemText[i][3] = MakeSs(c.extra ? c.extra : "");
            g_itemText[i][4] = MakeSs(c.setInventory);
            g_itemText[i][5] = MakeSs(c.title);
        }
        if (!g_view || !g_selector) { LOG_ERR("[storage] out of memory"); return false; }

        // The open path must never run without the close hook and the frame tick.
        const bool ok = Hook("WarehouseHandler", A.warehouseHandler, hkHandler, &oHandler) &&
                        Hook("StageClose", A.stageClose, hkStageClose, &oStageClose) &&
                        Hook("InputBlockSet", A.inputBlockSet, hkInputBlock, &oInputBlock) &&
                        Hook("ModeSwitch", A.modeSwitch, hkModeSwitch, &oModeSwitch);
        if (!ok)
        {
            LOG_ERR("[storage] a hook failed, so storage keys are turned off. Another storage mod may have hooked the same place.");
            farhook::RemoveAll();
            return false;
        }
        pad::Init();
        g_poller = CreateThread(nullptr, 0, Poller, nullptr, 0, nullptr);
        g_ready = true;
        return true;
    }

    bool Ready() { return g_ready.load(); }
    bool KeyWindowFound()
    {
        const int n = g_windowCount.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            if (IsWindow(g_windows[i].hwnd) && IsWindowVisible(g_windows[i].hwnd)) return true;
        return false;
    }
    int OpenStorage() { return g_open.load() ? g_openChest.load() : -1; }
    void PauseInput(unsigned ms) { g_pauseUntil = GetTickCount() + ms; }
    bool HidingKeys() { return g_hidingKeys.load(std::memory_order_relaxed); }

    Play PlayState()
    {
        if (!g_ready.load()) return Play::Unknown;
        const DWORD last = g_lastTick.load();
        if (!last || GetTickCount() - last > 1000) return Play::NotFree;
        return g_freePlay.load() ? Play::Free : Play::NotFree;
    }

    void Stop()
    {
        g_stop = true;
        if (g_poller)
        {
            WaitForSingleObject(g_poller, 1000);
            CloseHandle(g_poller);
            g_poller = nullptr;
        }
        RestoreGameWindows();
        pad::Shutdown();
    }
}

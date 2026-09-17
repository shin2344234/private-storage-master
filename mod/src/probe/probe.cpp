#include "probe/probe.h"

#include <Windows.h>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "probe/targets_2850.h"

// Probe 2. Probe 1's observation hooks, plus a remote open of each chest through
// the game's own StageChartUIControl event wrap and an owned close, per
// private/research/R3-storage-system.md sections 5 and 6.
//
// Keys. While Ctrl is held the probe swallows every F key, End and Delete
// before the game window sees them (mods that read the keyboard themselves,
// such as Crimson Route, still see them):
//   Ctrl+F1..F8  open or close Private Storage, Gatherables, Dresser,
//                Refrigerator, Collecting, Camp Straw, Bird Feed, Town Warehouse
//   Ctrl+F9      open or close the Kuku Pot
//   Ctrl+Delete  panic: post 0x0F for whatever the controller holds, drop the
//                IngameMenu phase and the input block
//   Ctrl+End     toggle the 0x12, 0x14 and 0x0D packets a natural open also sends
//                (on by default in this build)
//   Pause        dump inventory records, buckets, controller and phase

namespace psm::probe
{
    using namespace psm::probe2850;

    namespace
    {
        std::atomic<bool> g_stop{false};
        HANDLE g_poller = nullptr;

        uintptr_t Base() { return mem::Game().base; }
        uintptr_t Abs(uint32_t rva) { return Base() + rva; }
        uintptr_t Fn(int idx) { return Abs(kTargets[idx].rva); }
        unsigned long long U(uintptr_t v) { return static_cast<unsigned long long>(v); }

        // ------------------------------------------------------------ helpers
        // RVAs of the first frames that sit inside the game image, caller first.
        void StackLine(char* out, size_t cap)
        {
            void* frames[24] = {};
            const USHORT n = RtlCaptureStackBackTrace(1, 24, frames, nullptr);
            int w = snprintf(out, cap, "stack");
            int shown = 0;
            for (USHORT i = 0; i < n && shown < 10 && w < static_cast<int>(cap) - 16; ++i)
            {
                const uintptr_t f = reinterpret_cast<uintptr_t>(frames[i]);
                if (!mem::InImage(f)) continue;
                w += snprintf(out + w, cap - w, " %llX", U(f - Base()));
                ++shown;
            }
        }

        // A printable C string at p, or at *p.
        bool TryText(uintptr_t p, char* out, size_t cap)
        {
            out[0] = 0;
            if (!mem::Plausible(p)) return false;
            if (mem::ReadCString(p, out, cap) && strlen(out) >= 2) return true;
            uintptr_t q = 0;
            if (mem::ReadPtr(p, &q) && mem::ReadCString(q, out, cap) && strlen(out) >= 2) return true;
            out[0] = 0;
            return false;
        }

        // staticstringA const& -> object (node pointer) -> node -> char*.
        bool ReadSsRef(uintptr_t ref, char* out, size_t cap)
        {
            out[0] = 0;
            uintptr_t node = 0, text = 0;
            if (!mem::ReadPtr(ref, &node) || !mem::ReadPtr(node, &text)) return false;
            return mem::ReadCString(text, out, cap);
        }

        struct Budget
        {
            DWORD second = 0; int used = 0;
            bool Take(int perSecond)
            {
                const DWORD now = GetTickCount() / 1000;
                if (now != second) { second = now; used = 0; }
                return used++ < perSecond;
            }
        };

        // ------------------------------------------------------------ game structures
        // staticstringA node (R3A 2). A negative refcount makes the game skip every
        // addref and release, so these are never freed by it. The probe never frees
        // them either: queued copies keep the pointer.
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
            auto* node = static_cast<SsNode*>(VirtualAlloc(nullptr, sizeof(SsNode) + n + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!node) return nullptr;
            node->len = static_cast<uint32_t>(n);
            node->hash = 0xFFFFFFFF;
            node->ref = -1;
            node->flag = 0;
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
        using EntryResetFn = void(__fastcall*)(uintptr_t entry);
        using FindPanelFn  = uintptr_t(__fastcall*)(uintptr_t, const char*);
        using InputBlockFn = void(__fastcall*)(uintptr_t stageMgr, uint64_t* key, uint32_t mode);

        constexpr uint32_t kPlayerActor   = 0xA0100001;  // what every natural open carried
        constexpr uint8_t  kPhaseIngameMenu = 0x0E;
        constexpr uint8_t  kScreenIngame    = 0x10;

        struct Chest
        {
            const char* label;
            const char* setInventory;
            const char* title;
            const char* modalHash;   // sub 0x07 text of a natural 0x0D packet; nullptr: the chart sends no 0x0D
            const char* titleHash;   // sub 0x08 text of the 0x14 header: lookup3 of the lowercased title key
            const char* icon;        // sub 0x0B of the 0x14 header; nullptr: the chart leaves it empty
            const char* extra;       // another 0x15 command sent before SetInventory, or nullptr
        };
        // From the stage charts (private/research/r12_all_uicontrol.txt, R4-storage-containers.md).
        const Chest kChests[] = {
            { "Private Storage", "SetInventory(Character,Focus,True,Default;CampWareHouse,Focus,True,Default)",
              "SetWareHouseInventoryName(UI_WareHouse_CampStroage)", "197270237", "1494912655", "cd_icon_map_bank", nullptr },
            { "Gatherables", "SetInventory(Character,Focus,True;Housing_GatheredMaterials,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_HousingGatheredMaterials)", "2520550823", "1329171849", "cd_icon_map_bank", nullptr },
            { "Dresser", "SetInventory(Character,Focus,True;Housing_Dresser,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_HousingFurnitureDresser)", "2520550823", "1469492791", "cd_icon_map_bank", nullptr },
            { "Refrigerator", "SetInventory(Character,Focus,True;Housing_Refrigerator,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_HousingRefrigerator)", "2520550823", "295797541", "cd_icon_map_bank", nullptr },
            { "Collecting", "SetInventory(Character,Focus,True;Housing_Collecting,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_HousingCollecting)", "2520550823", "52739647", "cd_icon_map_bank", nullptr },
            // New in probe 2f: the camp feed bin, the bird feeder, and the town warehouse
            // NPC (its WareHouse function variant, without SetDonationFaction, which
            // needs the NPC actor, and without the 0x09 NPC packet).
            { "Camp Straw", "SetInventory(Character,Focus,True;CampStraw,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_CampStraw)", "2520550823", "502364378", "cd_icon_map_bank", nullptr },
            { "Bird Feed", "SetInventory(Character,Focus,True;BirdFeed,Focus,True)",
              "SetWareHouseInventoryName(UI_WareHouse_BirdFeed)", "2933432374", "3229017175", "cd_icon_map_bank", nullptr },
            { "Town Warehouse",
              "SetInventory(Character,Focus,True,Default;WareHouse,Focus,True;Wagon,NearWagon,False;PetAndVehicle,NearMercenary_Vehicle,False;Ship,Ship,False)",
              "SetWareHouseInventoryName(UI_WareHouse_Stroage)", nullptr, "2638607143", nullptr, "ShowPackageCampMoneyList()" },
            // Kuku inventory (key 13, 240 slots, moves both ways with Character). The
            // game shows it on its own KukuEnchantPanel at the Kuku pot NPC; here it goes
            // in the warehouse screen. Title UI_Inventory_KukuItemList ("Kuku Pot Bag",
            // lookup3 0xCEF81A05), icon from the Kuku NPC chart, straw modal text.
            { "Kuku Pot", "SetInventory(Character,Focus,True;Kuku,Focus,True)",
              "SetWareHouseInventoryName(UI_Inventory_KukuItemList)", "2520550823", "3472366085", "cd_icon_map_kukushop", nullptr },
        };
        const int kChestKeys[] = { VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9 };
        constexpr int kChestCount = static_cast<int>(sizeof kChests / sizeof kChests[0]);

        // ------------------------------------------------------------ captured / shared state
        std::atomic<uintptr_t> g_warehouse{0};   // UIGamePlayControlRootWarehouse2 instance
        std::atomic<uintptr_t> g_modeObj{0};     // UI phase manager, the mode switch argument

        // Requests from the poller, run on the main thread inside the mode switch.
        enum : int { kActNone = -1, kActClose = 100, kActPanic = 101 };
        std::atomic<int>  g_pending{kActNone};
        std::atomic<bool> g_fidelity{true};

        // Remote screen state. Written on the main thread; the atomics are read by the poller.
        std::atomic<bool>     g_remoteOpen{false};
        std::atomic<uint64_t> g_remoteId{0};
        std::atomic<bool>     g_saw15{false}, g_saw0E{false};
        int      g_remoteChest = -1;
        uint32_t g_remoteFrames = 0;
        uint64_t g_nextId = 900000000;  // live stage ids are small and count up from boot

        void* oHandler = nullptr, *oSetInventory = nullptr, *oMenuRequest = nullptr, *oModeSwitch = nullptr,
            *oItemDetail = nullptr, *oCounting = nullptr, *oWarehouseClose = nullptr, *oListener = nullptr,
            *oMoveCheck = nullptr, *oMoveSend = nullptr, *oStageClose = nullptr, *oInputBlock = nullptr, *oMoveDialogConfirm = nullptr;

        // InputBlock registry (ClientSequencerStageManager +0x200). The chart adds
        // {stage id, 2} on open and removes it on close; mode 2 applies the
        // ActionBlockGroup that keeps skills and movement out of the screen.
        std::atomic<uintptr_t> g_stageMgr{0};
        uintptr_t g_blockMgr = 0;       // main thread: where our entry was added
        uint64_t  g_blockKey = 0;

        using Fn4  = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
        using Fn6  = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
        using Fn12 = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                                            uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

        // ------------------------------------------------------------ packet dump
        void DumpPacket(const char* tag, uintptr_t pkt)
        {
            uint8_t kind = 0;
            if (!mem::Read8(pkt, &kind)) { LOG("%s   packet %llX unreadable", tag, U(pkt)); return; }
            uintptr_t subs = 0; uint32_t n = 0;
            mem::ReadPtr(pkt + 8, &subs);
            mem::Read32(pkt + 0x10, &n);
            LOG("%s   packet kind 0x%02X, %u sub-commands at %llX", tag, kind, n, U(subs));
            if (!subs || n > 64) return;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uintptr_t s = subs + 0x18ull * i;
                uint8_t sk = 0; uintptr_t args = 0; uint32_t an = 0;
                mem::Read8(s, &sk);
                mem::ReadPtr(s + 8, &args);
                mem::Read32(s + 0x10, &an);
                LOG("%s   sub %u kind 0x%02X, %u args", tag, i, sk, an);
                if (!args || an > 32) continue;
                for (uint32_t k = 0; k < an; ++k)
                {
                    uint64_t q[2] = {};
                    mem::ReadBytes(args + 0x10ull * k, q, sizeof q);
                    char t[200];
                    TryText(static_cast<uintptr_t>(q[1]), t, sizeof t);
                    LOG("%s     arg %u type %u value %016llX \"%s\"", tag, k, static_cast<unsigned>(q[0] & 0xFF), q[1], t);
                }
            }
        }

        // First type-5 argument of the first kind-0 sub-command, as a number (the stage id).
        bool PacketStageId(uintptr_t pkt, uint64_t* out)
        {
            uintptr_t subs = 0; uint32_t n = 0;
            if (!mem::ReadPtr(pkt + 8, &subs) || !mem::Read32(pkt + 0x10, &n) || n > 64) return false;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uintptr_t s = subs + 0x18ull * i;
                uint8_t sk = 0; uintptr_t args = 0; uint32_t an = 0;
                if (!mem::Read8(s, &sk) || sk != 0) continue;
                if (!mem::ReadPtr(s + 8, &args) || !mem::Read32(s + 0x10, &an) || !an) return false;
                uint64_t q[2] = {};
                if (!mem::ReadBytes(args, q, sizeof q)) return false;
                char t[32];
                if (!TryText(static_cast<uintptr_t>(q[1]), t, sizeof t)) return false;
                char* end = nullptr;
                *out = strtoull(t, &end, 10);
                return end && *end == 0;
            }
            return false;
        }

        void ControllerLine(const char* why, uintptr_t c)
        {
            if (!c) { LOG("[ctrl] %s: no warehouse controller seen yet", why); return; }
            uint64_t id = 0; uint8_t shown = 0, stagechart = 0, b352 = 0, wagon = 0, b439 = 0;
            uint32_t pending = 0;
            mem::Read64(c + 0x128, &id);
            mem::Read8(c + 0x130, &shown);
            mem::Read8(c + 0x34C, &stagechart);
            mem::Read8(c + 0x352, &b352);
            mem::Read8(c + 0x43A, &wagon);
            mem::Read8(c + 0x439, &b439);
            mem::Read32(c + 0x300, &pending);
            LOG("[ctrl] %s: %llX stage id %llu shown %u use-stagechart %u +352 %u wagon %u +439 %u entries(+300) %u", why, U(c), id,
                shown, stagechart, b352, wagon, b439, pending);
        }

        // ------------------------------------------------------------ remote open / close (main thread only)
        uintptr_t EventWrap()
        {
            uintptr_t mgr = 0, wrap = 0, vt = 0;
            if (!mem::ReadPtr(Abs(kEventMgrGlobal), &mgr) || !mem::ReadPtr(mgr + kEventMgrWrapOff, &wrap) || !mem::ReadPtr(wrap, &vt))
                return 0;
            return vt == Abs(kEventWrapVtable) ? wrap : 0;
        }

        // SEH wrappers keep game faults out of the frame that owns C++ state.
        bool PostGuarded(uintptr_t wrap, uintptr_t* view, uintptr_t* sel, uint64_t* id, Data* d)
        {
            __try { static_cast<PostFn>(reinterpret_cast<void*>(Fn(kEventPost)))(wrap, kPlayerActor, view, sel, id, d); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool PhaseGuarded(uintptr_t pm, uint8_t phase, bool on)
        {
            __try { static_cast<PhaseFn>(reinterpret_cast<void*>(Fn(kRequestPhase)))(pm, phase, on ? 1 : 0); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        SsNode* g_view = nullptr, *g_selector = nullptr;

        bool Post(uint8_t kind, uint64_t stageId, const SsNode* idText, Sub* extra, uint32_t extraCount)
        {
            const uintptr_t wrap = EventWrap();
            if (!wrap) { LOG_ERR("[remote] StageChartUIControl wrap not found (manager +%X, +%X)", kEventMgrGlobal, kEventMgrWrapOff); return false; }
            Arg idArg{}; idArg.type = 5; idArg.value = reinterpret_cast<uint64_t>(idText);
            Sub subs[4]{};
            subs[0].kind = 0x00; subs[0].args = &idArg; subs[0].count = subs[0].cap = 1;
            for (uint32_t i = 0; i < extraCount && i < 3; ++i) subs[1 + i] = extra[i];
            Data d{};
            d.kind = kind; d.subs = subs; d.count = d.cap = 1 + (extraCount < 3 ? extraCount : 3);
            uintptr_t view = reinterpret_cast<uintptr_t>(g_view), sel = reinterpret_cast<uintptr_t>(g_selector);
            uint64_t id = stageId;
            const bool ok = PostGuarded(wrap, &view, &sel, &id, &d);
            LOG("[remote] post 0x%02X stage %llu (%u sub-commands) %s", kind, stageId, d.count, ok ? "queued" : "FAULTED");
            return ok;
        }

        bool InputBlockGuarded(uintptr_t mgr, uint64_t key, uint32_t mode)
        {
            __try { static_cast<InputBlockFn>(reinterpret_cast<void*>(Fn(kInputBlockSet)))(mgr, &key, mode); return true; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // The player's ClientSequencerStageManager component. Taken from the
        // InputBlock hook when a natural chest has run, otherwise searched for in
        // the user actor, the controlled character and their component tables.
        uintptr_t StageManager()
        {
            const uintptr_t vt = Abs(kStageMgrVtable);
            uintptr_t known = g_stageMgr.load(), v = 0;
            if (known && mem::ReadPtr(known, &v) && v == vt) return known;
            uintptr_t g = 0, mgr = 0, user = 0, ch = 0;
            if (!mem::ReadPtr(Abs(kActorMgrGlobal), &g) || !mem::ReadPtr(g + 0x30, &mgr) || !mem::ReadPtr(mgr + 0x58, &user)) return 0;
            mem::ReadPtr(user + 0xD8, &ch);
            const uintptr_t owners[2] = {user, ch};
            const char* const names[2] = {"user actor", "character"};
            for (int o = 0; o < 2; ++o)
            {
                if (!owners[o]) continue;
                uintptr_t comps = 0;
                const uintptr_t tables[2] = {owners[o], mem::ReadPtr(owners[o] + 0x68, &comps) ? comps : 0};
                for (int t = 0; t < 2; ++t)
                {
                    if (!tables[t]) continue;
                    for (unsigned off = 0; off < 0x800; off += 8)
                    {
                        uintptr_t q = 0;
                        if (!mem::ReadPtr(tables[t] + off, &q) || !mem::ReadPtr(q, &v) || v != vt) continue;
                        LOG("[block] stage manager %llX found at %s %llX %s+0x%X", U(q), names[o], U(owners[o]), t ? "[+0x68]" : "", off);
                        g_stageMgr = q;
                        return q;
                    }
                }
            }
            LOG_ERR("[block] stage manager not found (user %llX, character %llX)", U(user), U(ch));
            return 0;
        }

        void BlockInput(uint64_t id)
        {
            const uintptr_t mgr = StageManager();
            if (!mgr) return;
            const bool ok = InputBlockGuarded(mgr, id, 2);
            LOG("[block] add stage %llu mode 2 on %llX: %s", id, U(mgr), ok ? "done" : "FAULTED");
            if (ok) { g_blockMgr = mgr; g_blockKey = id; }
        }

        void ReleaseInput()
        {
            if (!g_blockMgr) return;
            const bool ok = InputBlockGuarded(g_blockMgr, g_blockKey, 0);
            LOG("[block] remove stage %llu on %llX: %s", g_blockKey, U(g_blockMgr), ok ? "done" : "FAULTED");
            g_blockMgr = 0;
            g_blockKey = 0;
        }

        void DropRemote()
        {
            ReleaseInput();
            g_remoteOpen = false;
            g_remoteChest = -1;
        }

        bool GateOpen(uintptr_t pm, char* why, size_t cap)
        {
            uint8_t mode = 0, screen = 0, blocked = 0;
            uint32_t queued = 0;
            uintptr_t proc = 0;
            mem::Read8(pm + 0x28, &mode);
            mem::Read8(pm + 0x29, &screen);
            mem::Read32(pm + 0x70, &queued);
            if (mem::ReadPtr(pm + 0x10, &proc)) mem::Read8(proc + 0xA0, &blocked);
            snprintf(why, cap, "mode %u screen 0x%02X queued %u events blocked %u", mode, screen, queued, blocked);
            return mode == 4 && screen == kScreenIngame && queued == 0 && proc && blocked == 0;
        }

        void CloseRemote(uintptr_t pm, const char* reason)
        {
            const uint64_t id = g_remoteId.load();
            char text[32]; snprintf(text, sizeof text, "%llu", id);
            LOG("[remote] close %s (stage %llu, %u frames after open): %s", g_remoteChest >= 0 ? kChests[g_remoteChest].label : "?", id,
                g_remoteFrames, reason);
            Post(0x0F, id, MakeSs(text), nullptr, 0);
            LOG("[remote] phase IngameMenu off %s", PhaseGuarded(pm, kPhaseIngameMenu, false) ? "requested" : "FAULTED");
            DropRemote();
        }

        void OpenRemote(uintptr_t pm, int chest)
        {
            const Chest& c = kChests[chest];
            char why[128];
            if (!GateOpen(pm, why, sizeof why)) { LOG("[remote] %s refused, not in free gameplay: %s", c.label, why); return; }
            if (!EventWrap()) { LOG_ERR("[remote] %s refused: event wrap not found", c.label); return; }

            const uint64_t id = ++g_nextId;
            char idText[32]; snprintf(idText, sizeof idText, "%llu", id);
            SsNode* idNode = MakeSs(idText);
            ControllerLine("before open", g_warehouse.load());
            LOG("[remote] open %s, stage %llu, fidelity %s, gate %s", c.label, id, g_fidelity.load() ? "on" : "off", why);

            g_remoteId = id;
            g_saw15 = false;
            g_saw0E = false;
            g_remoteChest = chest;
            g_remoteFrames = 0;
            g_remoteOpen = true;

            // Chart order: InputBlock, then the phase, then the UIControl nodes.
            // RequestPhase blocks event processing until the mode switch applies it.
            BlockInput(id);
            LOG("[remote] phase IngameMenu on %s", PhaseGuarded(pm, kPhaseIngameMenu, true) ? "requested" : "FAULTED");

            if (g_fidelity.load())
            {
                Post(0x12, id, idNode, nullptr, 0);
                // 0x14 header: sub 0x0B type 1 icon (empty in the NPC charts), sub 0x08
                // type 9 title hash. Routed to vt+0x4A0.
                Arg iconArg{};  iconArg.type = 1;  iconArg.value = reinterpret_cast<uint64_t>(MakeSs(c.icon ? c.icon : ""));
                Arg titleArg{}; titleArg.type = 9; titleArg.value = reinterpret_cast<uint64_t>(MakeSs(c.titleHash));
                Sub header[2]{};
                header[0].kind = 0x0B; header[0].args = c.icon ? &iconArg : nullptr; header[0].count = header[0].cap = c.icon ? 1 : 0;
                header[1].kind = 0x08; header[1].args = &titleArg; header[1].count = header[1].cap = 1;
                Post(0x14, id, idNode, header, 2);
                if (c.modalHash)
                {
                    Arg hashArg{}; hashArg.type = 9; hashArg.value = reinterpret_cast<uint64_t>(MakeSs(c.modalHash));
                    Sub s7{}; s7.kind = 0x07; s7.args = &hashArg; s7.count = s7.cap = 1;
                    Post(0x0D, id, idNode, &s7, 1);
                }
            }

            Arg extraArg{}; extraArg.type = 5; extraArg.value = reinterpret_cast<uint64_t>(MakeSs(c.extra ? c.extra : ""));
            Arg invArg{};   invArg.type = 5;   invArg.value = reinterpret_cast<uint64_t>(MakeSs(c.setInventory));
            Arg nameArg{};  nameArg.type = 5;  nameArg.value = reinterpret_cast<uint64_t>(MakeSs(c.title));
            Sub cmds[3]{};
            uint32_t n = 0;
            if (c.extra) { cmds[n].kind = 0x0E; cmds[n].args = &extraArg; cmds[n].count = cmds[n].cap = 1; ++n; }
            cmds[n].kind = 0x0E; cmds[n].args = &invArg;  cmds[n].count = cmds[n].cap = 1; ++n;
            cmds[n].kind = 0x0E; cmds[n].args = &nameArg; cmds[n].count = cmds[n].cap = 1; ++n;
            Post(0x15, id, idNode, cmds, n);
            Post(0x0E, id, idNode, nullptr, 0);
        }

        void Panic(uintptr_t pm)
        {
            const uintptr_t c = g_warehouse.load();
            ControllerLine("panic", c);
            uint64_t id = 0;
            if (c) mem::Read64(c + 0x128, &id);
            if (!id) id = g_remoteId.load();
            char text[32]; snprintf(text, sizeof text, "%llu", id);
            Post(0x0F, id, MakeSs(text), nullptr, 0);
            LOG("[remote] panic: phase IngameMenu off %s", PhaseGuarded(pm, kPhaseIngameMenu, false) ? "requested" : "FAULTED");
            DropRemote();
        }

        void PollViews();

        // Runs after the original mode switch, once per frame.
        void MainThreadTick(uintptr_t pm)
        {
            static unsigned tick = 0;
            if (++tick % 6 == 0) PollViews();

            int act = g_pending.exchange(kActNone);
            if (act != kActNone)
            {
                if (act == kActPanic) Panic(pm);
                else if (act == kActClose) { if (g_remoteOpen.load()) CloseRemote(pm, "owned close"); }
                else if (act >= 0 && act < kChestCount)
                {
                    if (g_remoteOpen.load())
                    {
                        const bool same = act == g_remoteChest;
                        CloseRemote(pm, same ? "toggle key" : "another chest key, press it again to open that one");
                    }
                    else OpenRemote(pm, act);
                }
            }

            if (!g_remoteOpen.load()) return;
            ++g_remoteFrames;
            uint8_t screen = 0, mode = 0;
            mem::Read8(pm + 0x29, &screen);
            mem::Read8(pm + 0x28, &mode);
            // Leaving the game with the screen open (mode 5 then 7 on quit, probe 2h
            // session) tears down the stage manager and the UI. Forget the screen
            // without posting or touching the old InputBlock registry.
            if (mode != 4)
            {
                LOG("[remote] game mode is now %u with %s open; forgetting it", mode, g_remoteChest >= 0 ? kChests[g_remoteChest].label : "?");
                g_blockMgr = 0;
                g_blockKey = 0;
                g_remoteOpen = false;
                g_remoteChest = -1;
                return;
            }
            if (g_remoteFrames == 150)
            {
                if (!g_saw0E.load()) { CloseRemote(pm, "rollback: the controller never received the 0x0E"); return; }
                if (screen != kPhaseIngameMenu) { CloseRemote(pm, "rollback: the phase never reached IngameMenu"); return; }
                LOG("[remote] settled: 0x15 %s, 0x0E seen, screen 0x%02X", g_saw15.load() ? "seen" : "NOT seen", screen);
                ControllerLine("settled", g_warehouse.load());
            }
            if (g_remoteFrames > 150)
            {
                uint64_t cur = 0;
                const uintptr_t c = g_warehouse.load();
                if (c && mem::Read64(c + 0x128, &cur) && cur != g_remoteId.load())
                {
                    LOG("[remote] controller now holds stage %llu, not ours (%llu). Dropping ownership without posting. Screen 0x%02X.",
                        cur, g_remoteId.load(), screen);
                    DropRemote();
                }
                else if (screen == kScreenIngame)
                {
                    LOG("[remote] back in gameplay without the owned close running. Dropping ownership.");
                    DropRemote();
                }
            }
        }

        // ------------------------------------------------------------ detours
        uintptr_t __fastcall hkHandler(uintptr_t self, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
        {
            uintptr_t vt = 0;
            mem::ReadPtr(self, &vt);
            const bool warehouse = vt == Abs(kWarehouseVtable);
            if (warehouse && g_warehouse.load() != self)
            {
                g_warehouse = self;
                LOG("[handler] warehouse controller %llX", U(self));
            }
            uint8_t kind = 0;
            uint64_t id = 0;
            const bool haveId = r9 && mem::Read8(r9, &kind) && PacketStageId(r9, &id);
            const bool ours = haveId && g_remoteOpen.load() && id == g_remoteId.load();
            if (ours && kind == 0x15) g_saw15 = true;
            if (ours && kind == 0x0E) g_saw0E = true;

            static Budget b;
            if (b.Take(40))
            {
                char st[256]; StackLine(st, sizeof st);
                LOG("[handler] this %llX (vtable +%llX) rdx %llX packet %llX kind 0x%02X stage %llu%s | %s", U(self), U(mem::Rva(vt)), U(rdx),
                    U(r9), kind, id, ours ? " (ours)" : "", st);
                if (r9) DumpPacket("[handler]", r9);
            }
            const uintptr_t r = static_cast<Fn4>(oHandler)(self, rdx, r8, r9);
            if (warehouse && (kind == 0x0E || kind == 0x0F || kind == 0x15)) ControllerLine("after packet", self);
            return r;
        }

        uintptr_t __fastcall hkSetInventory(uintptr_t self, uintptr_t text, uintptr_t r8, uintptr_t r9)
        {
            char s[256] = {};
            mem::ReadCString(text, s, sizeof s);
            LOG("[setinv] this %llX \"%s\"", U(self), s);
            return static_cast<Fn4>(oSetInventory)(self, text, r8, r9);
        }

        uintptr_t __fastcall hkMenuRequest(uintptr_t obj, uintptr_t a, uintptr_t b, uintptr_t flag)
        {
            char sa[160] = {}, sb[160] = {};
            mem::ReadCString(a, sa, sizeof sa);
            mem::ReadCString(b, sb, sizeof sb);
            LOG("[menureq] obj %llX \"%s\" \"%s\" flag %u", U(obj), sa, sb, static_cast<unsigned>(flag & 0xFF));
            return static_cast<Fn4>(oMenuRequest)(obj, a, b, flag);
        }

        uint8_t g_modePrev[0x58] = {};
        bool    g_modeHave = false;
        uintptr_t __fastcall hkModeSwitch(uintptr_t obj, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
        {
            const uintptr_t r = static_cast<Fn4>(oModeSwitch)(obj, rdx, r8, r9);
            uint8_t now[0x58] = {};
            if (mem::ReadBytes(obj + 0x20, now, sizeof now))
            {
                if (g_modeObj.load() != obj)
                {
                    uintptr_t root = 0, pm = 0;
                    if (mem::ReadPtr(Abs(kUiRootGlobal), &root)) mem::ReadPtr(root + 0x98, &pm);
                    LOG("[mode] mode object %llX (root+0x98 holds %llX%s)", U(obj), U(pm), pm == obj ? ", same" : ", DIFFERENT");
                    g_modeObj = obj;
                    g_modeHave = false;
                }
                if (!g_modeHave || memcmp(now, g_modePrev, sizeof now) != 0)
                {
                    static Budget bud;
                    if (bud.Take(20))
                    {
                        char hex[0x58 * 3 + 1] = {};
                        for (int i = 0; i < 0x58; ++i) snprintf(hex + i * 3, 4, "%02X ", now[i]);
                        LOG("[mode] +20: %s", hex);
                    }
                    memcpy(g_modePrev, now, sizeof now);
                    g_modeHave = true;
                }
            }
            MainThreadTick(obj);
            return r;
        }

        // These openers take stack arguments (+0xD968E0 has eight). A detour that
        // does anything after the call must forward the stack slots too, or the
        // callee reads the detour's own frame: that was the probe 2c crash.
        uintptr_t __fastcall hkItemDetail(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
                                          uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12)
        {
            LOG("[modal] item detail opener (%llX %llX)", U(a1), U(a2));
            return static_cast<Fn12>(oItemDetail)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
        }
        uintptr_t __fastcall hkCounting(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
                                        uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12)
        {
            char st[256]; StackLine(st, sizeof st);
            const uintptr_t r = static_cast<Fn12>(oCounting)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
            LOG("[modal] counting opener (%llX %llX %llX %llX | %llX %llX %llX %llX) returned %llX | %s", U(a1), U(a2), U(a3), U(a4), U(a5),
                U(a6), U(a7), U(a8), U(r), st);
            return r;
        }

        // Warehouse2 slot 116 (+0xCB0190): a dialog's result. For the quantity
        // dialog it runs +0xCA6CB0, which needs dialog == ctrl+0x2D0, a focused
        // item at ctrl+0x1C0 and a move rule index at ctrl+0x234.
        uintptr_t __fastcall hkMoveDialogConfirm(uintptr_t ctrl, uintptr_t dialog, uintptr_t flag, uintptr_t r9, uintptr_t a5, uintptr_t a6,
                                                 uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12)
        {
            uintptr_t d2d0 = 0, focus = 0;
            uint32_t rule = 0, list = 0;
            mem::Read64(ctrl + 0x2D0, &d2d0);
            mem::Read64(ctrl + 0x1C0, &focus);
            mem::Read32(ctrl + 0x234, &rule);
            mem::Read32(ctrl + 0x230, &list);
            uint64_t count = 0; uint16_t slotKey = 0;
            if (focus) { mem::Read16(focus + 0x838, &slotKey); mem::Read64(focus + 0x840, &count); }
            char st[256]; StackLine(st, sizeof st);
            LOG("[confirm] slot 116 ctrl %llX dialog %llX (ctrl+2D0 %llX%s) flag %u r9 %llX focus %llX item %04X count %lld rule %d list %u | %s",
                U(ctrl), U(dialog), U(d2d0), dialog == d2d0 ? ", match" : ", NO MATCH", static_cast<unsigned>(flag & 0xFF), U(r9), U(focus),
                slotKey, static_cast<long long>(count), static_cast<int>(rule), list, st);
            return static_cast<Fn12>(oMoveDialogConfirm)(ctrl, dialog, flag, r9, a5, a6, a7, a8, a9, a10, a11, a12);
        }

        // The reset loop at the head of +0xCAB6B0, run by hand for our own screen.
        bool ResetEntriesGuarded(uintptr_t self)
        {
            __try
            {
                const auto reset = static_cast<EntryResetFn>(reinterpret_cast<void*>(Fn(kCloseEntryReset)));
                auto* count = reinterpret_cast<uint32_t*>(self + 0x300);
                for (uint32_t i = 0; i < *count; ++i)
                    reset(*reinterpret_cast<uintptr_t*>(self + 0x2F8) + 0x108ull * i);
                *count = 0;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // Warehouse2 close (+0xCAB6B0). For a screen the probe opened there is no
        // stage to send "Close" to, so the probe runs the reset loop and queues its
        // own 0x0F and phase off for the next mode switch.
        uintptr_t __fastcall hkWarehouseClose(uintptr_t self, uintptr_t stopStage, uintptr_t r8, uintptr_t r9)
        {
            uint64_t cur = 0;
            mem::Read64(self + 0x128, &cur);
            char st[256]; StackLine(st, sizeof st);
            const bool ours = g_remoteOpen.load() && cur == g_remoteId.load();
            LOG("[close] warehouse close this %llX stop %u stage %llu%s | %s", U(self), static_cast<unsigned>(stopStage & 0xFF), cur,
                ours ? " (ours, handled by the probe)" : "", st);
            ControllerLine("at close", self);
            if (ours)
            {
                LOG("[close] reset loop %s", ResetEntriesGuarded(self) ? "done" : "FAULTED");
                g_pending = kActClose;
                return 0;
            }
            return static_cast<Fn4>(oWarehouseClose)(self, stopStage, r8, r9);
        }

        // Every CommonRoot's StageChartUIControl listener (+0xDBA720). Logs events
        // for WareHouseView, and dumps the kinds that never reach slot 144.
        uintptr_t __fastcall hkListener(uintptr_t root, uintptr_t actor, uintptr_t view, uintptr_t sel, uintptr_t stageId, uintptr_t data)
        {
            char v[64];
            uintptr_t vt = 0;
            if (mem::ReadPtr(root, &vt) && vt == Abs(kWarehouseVtable) && ReadSsRef(view, v, sizeof v) && strcmp(v, "WareHouseView") == 0)
            {
                uint8_t kind = 0; uint64_t id = 0;
                mem::Read8(data, &kind);
                mem::Read64(stageId, &id);
                static Budget b;
                if (b.Take(40))
                {
                    const bool slot144 = kind >= 0x0C && kind != 0x10 && kind != 0x11 && kind != 0x14;
                    LOG("[listener] warehouse root %llX actor %X kind 0x%02X stage %llu%s", U(root), static_cast<unsigned>(actor), kind, id,
                        slot144 ? "" : (kind == 0x09 ? " -> slot 145, wagon mode" : " -> not slot 144, dumped"));
                    if (!slot144) DumpPacket("[listener]", data);
                }
            }
            return static_cast<Fn6>(oListener)(root, actor, view, sel, stageId, data);
        }

        // "Close" to a stage (+0x5F8090). Esc in the warehouse (MoveItem slot 113,
        // +0xCA5383) calls this directly with &controller+0x128; probe 2's first
        // session showed the +0xCAB6B0 routine is not on that path.
        uintptr_t __fastcall hkStageClose(uintptr_t rcx, uintptr_t idPtr, uintptr_t root, uintptr_t name)
        {
            uint64_t id = 0;
            char n[16] = {};
            if (g_remoteOpen.load() && mem::Read64(idPtr, &id) && id == g_remoteId.load() && mem::ReadCString(name, n, sizeof n) &&
                strcmp(n, "Close") == 0)
            {
                uintptr_t vt = 0;
                const uintptr_t ctrl = idPtr - 0x128;
                mem::ReadPtr(ctrl, &vt);
                char st[256]; StackLine(st, sizeof st);
                LOG("[close] \"Close\" for our stage %llu from %s %llX | %s", id, vt == Abs(kWarehouseVtable) ? "warehouse" : "unknown object",
                    U(ctrl), st);
                g_pending = kActClose;
                return 0;
            }
            return static_cast<Fn4>(oStageClose)(rcx, idPtr, root, name);
        }

        uintptr_t __fastcall hkInputBlock(uintptr_t mgr, uintptr_t keyPtr, uintptr_t mode, uintptr_t r9)
        {
            uint64_t key = 0;
            mem::Read64(keyPtr, &key);
            uintptr_t vt = 0;
            if (mem::ReadPtr(mgr, &vt) && vt == Abs(kStageMgrVtable) && g_stageMgr.load() != mgr)
            {
                g_stageMgr = mgr;
                LOG("[block] stage manager %llX (from the game's own call)", U(mgr));
            }
            uint8_t level = 0;
            mem::Read8(mgr + 0x220, &level);
            LOG("[block] InputBlock %llX stage %llu mode %u (level before %u)", U(mgr), key, static_cast<unsigned>(mode & 0xFF), level);
            return static_cast<Fn4>(oInputBlock)(mgr, keyPtr, mode, r9);
        }

        std::atomic<DWORD> g_bucketDiffAt{0};
        std::atomic<bool>  g_bucketFull{false};

        uintptr_t __fastcall hkMoveCheck(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
                                         uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12)
        {
            char st[256]; StackLine(st, sizeof st);
            LOG("[move] check holder %llX err@%llX A %X B %X a5 %llX a6 %llX a7 %llX a8 %llX a9 %llX a10 %llX | %s", U(a1), U(a2),
                static_cast<unsigned>(a3), static_cast<unsigned>(a4), U(a5), U(a6), U(a7), U(a8), U(a9), U(a10), st);
            const uintptr_t r = static_cast<Fn12>(oMoveCheck)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
            uint32_t err = 0;
            mem::Read32(a2, &err);
            LOG("[move] check returned %llX, error %08X%s", U(r), err, err ? "" : " (none)");
            g_bucketDiffAt = GetTickCount() + 1500;
            return r;
        }

        uintptr_t __fastcall hkMoveSend(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6,
                                        uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11, uintptr_t a12)
        {
            LOG("[move] send 0xAFE: %llX %llX %llX %llX %llX %llX %llX %llX %llX %llX", U(a1), U(a2), U(a3), U(a4), U(a5), U(a6), U(a7),
                U(a8), U(a9), U(a10));
            const uintptr_t r = static_cast<Fn12>(oMoveSend)(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
            LOG("[move] send returned %llX", U(r));
            return r;
        }

        // ------------------------------------------------------------ poller
        uintptr_t FindPanelGuarded(uintptr_t pm, const char* name)
        {
            __try { return static_cast<FindPanelFn>(reinterpret_cast<void*>(Fn(kFindPanelTop)))(pm, name); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }

        // GameMain UIWindow, R3E: [[[[base+0x6C2DA48]+0x68]+0x78]+0].
        uintptr_t GameMainWindow()
        {
            uintptr_t root = 0, sys = 0, arr = 0, win = 0;
            if (!mem::ReadPtr(Abs(kUiRootGlobal), &root) || !mem::ReadPtr(root + 0x68, &sys) || !mem::ReadPtr(sys + 0x78, &arr) ||
                !mem::ReadPtr(arr, &win))
                return 0;
            return win;
        }

        const char* const kViews[] = {
            "WareHouseView", "MainMenuView2", "WorldMapView", "ModalMessageView", "HousingManagementPanel",
            "KeyGuidePanel", "ItemGiftInventory", "NpcStoreBuyView", "NpcStoreSellView",
        };
        int g_viewLast[sizeof kViews / sizeof kViews[0]] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
        std::atomic<bool> g_warehouseOpen{false};

        void DumpController(uintptr_t c)
        {
            LOG("[ctrl] warehouse controller %llX, non-zero qwords up to +0x440:", U(c));
            char line[900]; int w = 0;
            for (unsigned off = 0; off < 0x440; off += 8)
            {
                uint64_t q = 0;
                if (!mem::Read64(c + off, &q) || !q) continue;
                w += snprintf(line + w, sizeof line - w, " +%X=%llX", off, q);
                if (w > 760) { LOG("[ctrl] %s", line); w = 0; }
            }
            if (w) LOG("[ctrl] %s", line);
        }

        // Inventory record name, **(rec+8).
        bool RecordName(uint16_t index, char* out, size_t cap)
        {
            out[0] = 0;
            uintptr_t mgr = 0, arr = 0, rec = 0, node = 0, text = 0;
            uint32_t n = 0;
            if (!mem::ReadPtr(Abs(kInvMgrGlobal), &mgr) || !mem::Read32(mgr + 0x08, &n) || index >= n) return false;
            if (!mem::ReadPtr(mgr + 0x58, &arr) || !mem::ReadPtr(arr + 8ull * index, &rec)) return false;
            if (!mem::ReadPtr(rec + 8, &node) || !mem::ReadPtr(node, &text)) return false;
            return mem::ReadCString(text, out, cap);
        }

        void DumpInventoryInfo()
        {
            uintptr_t mgr = 0;
            if (!mem::ReadPtr(Abs(kInvMgrGlobal), &mgr)) { LOG("[inv] manager not created (global +%X)", kInvMgrGlobal); return; }
            uint32_t n = 0; uintptr_t arr = 0;
            mem::Read32(mgr + 0x08, &n);
            mem::ReadPtr(mgr + 0x58, &arr);
            LOG("[inv] manager %llX count %u records %llX", U(mgr), n, U(arr));
            if (!arr || n > 256) return;
            for (uint32_t i = 0; i < n; ++i)
            {
                uintptr_t info = 0;
                mem::ReadPtr(arr + 8ull * i, &info);
                uint16_t base = 0, max = 0;
                if (info) { mem::Read16(info + 0x48, &base); mem::Read16(info + 0x4A, &max); }
                char name[96];
                RecordName(static_cast<uint16_t>(i), name, sizeof name);
                LOG("[inv]   [%2u] record %llX default %u max %u \"%s\"", i, U(info), base, max, name);
            }
        }

        // Controlled character's holder, R3C 5.
        uintptr_t PlayerHolder(uintptr_t* chOut)
        {
            uintptr_t g = 0, mgr = 0, user = 0, ch = 0, comp = 0, holder = 0, owner = 0;
            if (!mem::ReadPtr(Abs(kActorMgrGlobal), &g) || !mem::ReadPtr(g + 0x30, &mgr) || !mem::ReadPtr(mgr + 0x58, &user) ||
                !mem::ReadPtr(user + 0xD8, &ch) || !mem::ReadPtr(ch + 0x68, &comp) || !mem::ReadPtr(comp + 0xB8, &holder))
                return 0;
            if (chOut) *chOut = ch;
            if (!mem::ReadPtr(holder + 8, &owner) || owner != ch) return 0;
            return holder;
        }

        struct BucketSnap { uint16_t index, used, cap; };
        BucketSnap g_snap[128];
        int g_snapCount = -1;

        // ItemInfo manager (+0x6C2E2E8, read by +0x384DF0): +0x08 count, +0x58
        // record pointers (null until loaded). Record: name at **(rec+8) like
        // InventoryInfo, category byte +0xA3 (compared by the pushable test
        // +0x1A153B0 against the u8 in inventory pushable/excluded lists).
        uintptr_t ItemRecord(uint16_t index)
        {
            uintptr_t mgr = 0, arr = 0, rec = 0;
            uint32_t n = 0;
            if (!mem::ReadPtr(Abs(kItemMgrGlobal), &mgr) || !mem::Read32(mgr + 0x08, &n) || index >= n) return 0;
            if (!mem::ReadPtr(mgr + 0x58, &arr) || !mem::ReadPtr(arr + 8ull * index, &rec)) return 0;
            return rec;
        }

        bool ItemName(uintptr_t rec, char* out, size_t cap)
        {
            out[0] = 0;
            uintptr_t node = 0, text = 0;
            return rec && mem::ReadPtr(rec + 8, &node) && mem::ReadPtr(node, &text) && mem::ReadCString(text, out, cap);
        }

        void DumpItemCategories()
        {
            uintptr_t mgr = 0;
            uint32_t n = 0;
            if (!mem::ReadPtr(Abs(kItemMgrGlobal), &mgr) || !mem::Read32(mgr + 0x08, &n) || n > 0x10000)
            {
                LOG("[items] item manager not readable (global +%X)", kItemMgrGlobal);
                return;
            }
            uint32_t perCat[256] = {}, loaded = 0;
            char sample[256][48] = {};
            uint32_t gearCats[256] = {};
            for (uint32_t i = 0; i < n; ++i)
            {
                const uintptr_t rec = ItemRecord(static_cast<uint16_t>(i));
                uint8_t cat = 0;
                if (!rec || !mem::Read8(rec + 0xA3, &cat)) continue;
                ++loaded;
                ++perCat[cat];
                char name[96];
                if (!ItemName(rec, name, sizeof name)) continue;
                if (!sample[cat][0]) snprintf(sample[cat], sizeof sample[cat], "%s", name);
                if (strstr(name, "AbyssGear") && strncmp(name, "Recipe", 6) != 0) ++gearCats[cat];
                if ((cat == 102 || cat == 103) && perCat[cat] <= 12) LOG("[items]   category %u: [%u] %s", cat, i, name);
            }
            LOG("[items] item manager %llX, %u records, %u loaded", U(mgr), n, loaded);
            for (int c = 0; c < 256; ++c)
                if (perCat[c])
                    LOG("[items] category %3d: %5u items, Abyss gears %u, e.g. %s", c, perCat[c], gearCats[c], sample[c]);
        }

        // What a bucket holds, by category, with a few names.
        void DumpBucketItems(const char* want)
        {
            uintptr_t ch = 0;
            const uintptr_t holder = PlayerHolder(&ch);
            uintptr_t arr = 0; uint32_t n = 0;
            if (!holder || !mem::ReadPtr(holder + 0x18, &arr) || !mem::Read32(holder + 0x20, &n) || n > 128) return;
            for (uint32_t b = 0; b < n; ++b)
            {
                uintptr_t bk = 0, slots = 0;
                uint16_t index = 0, used = 0;
                uint32_t size = 0;
                if (!mem::ReadPtr(arr + 8ull * b, &bk) || !mem::Read16(bk + 0x10, &index) || !mem::Read16(bk + 0x12, &used)) continue;
                char bname[64];
                if (!RecordName(index, bname, sizeof bname) || strcmp(bname, want) != 0) continue;
                mem::ReadPtr(bk, &slots);
                mem::Read32(bk + 0x08, &size);
                uint32_t perCat[256] = {}, shown = 0, filled = 0;
                for (uint32_t k = 0; k < (size & 0xFFFF) && k < 1460; ++k)
                {
                    uint16_t item = 0xFFFF;
                    int64_t count = 0;
                    if (!mem::Read16(slots + 0xC8ull * k + 0x08, &item) || item == 0xFFFF) continue;
                    mem::ReadBytes(slots + 0xC8ull * k + 0x10, &count, sizeof count);
                    if (count <= 0) continue;
                    ++filled;
                    const uintptr_t rec = ItemRecord(item);
                    uint8_t cat = 0;
                    if (rec) mem::Read8(rec + 0xA3, &cat);
                    ++perCat[cat];
                    char iname[96] = "?";
                    ItemName(rec, iname, sizeof iname);
                    if (shown++ < 15) LOG("[bucket-items] %s slot %u item %u x%lld category %u %s", want, k, item, static_cast<long long>(count), cat, iname);
                }
                LOG("[bucket-items] %s: used %u, %u filled slots of %u", want, used, filled, size & 0xFFFF);
                for (int c = 0; c < 256; ++c)
                    if (perCat[c]) LOG("[bucket-items] %s category %d: %u", want, c, perCat[c]);
            }
        }

        void DumpBuckets(bool diffOnly)
        {
            uintptr_t ch = 0;
            const uintptr_t holder = PlayerHolder(&ch);
            if (!holder) { if (!diffOnly) LOG("[bucket] player holder not found (character %llX)", U(ch)); return; }
            uintptr_t arr = 0; uint32_t n = 0;
            if (!mem::ReadPtr(holder + 0x18, &arr) || !mem::Read32(holder + 0x20, &n) || n > 128) return;
            if (!diffOnly) LOG("[bucket] character %llX holder %llX (%s), %u buckets", U(ch), U(holder), mem::RttiShort(holder) ? mem::RttiShort(holder) : "?", n);
            BucketSnap now[128]{};
            for (uint32_t i = 0; i < n; ++i)
            {
                uintptr_t bk = 0;
                if (!mem::ReadPtr(arr + 8ull * i, &bk)) continue;
                mem::Read16(bk + 0x10, &now[i].index);
                mem::Read16(bk + 0x12, &now[i].used);
                mem::Read16(bk + 0x14, &now[i].cap);
                const bool changed = g_snapCount != static_cast<int>(n) || memcmp(&now[i], &g_snap[i], sizeof now[i]) != 0;
                if (diffOnly && !changed) continue;
                char name[64]; RecordName(now[i].index, name, sizeof name);
                uint32_t size = 0; int16_t granted = 0;
                mem::Read32(bk + 0x08, &size);
                mem::ReadBytes(bk + 0x18, &granted, sizeof granted);
                if (diffOnly)
                    LOG("[bucket] %-28s used %u -> %u, capacity %u -> %u", name, i < 128 && g_snapCount > 0 ? g_snap[i].used : 0, now[i].used,
                        g_snapCount > 0 ? g_snap[i].cap : 0, now[i].cap);
                else
                    LOG("[bucket]   [%2u] index %2u %-28s used %u capacity %u slots %u granted %d", i, now[i].index, name, now[i].used,
                        now[i].cap, size & 0xFFFF, granted);
            }
            memcpy(g_snap, now, sizeof now);
            g_snapCount = static_cast<int>(n);
        }

        bool GameInFront()
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            return pid == GetCurrentProcessId();
        }

        bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

        // Main thread only: the panel lookup walks structures the UI thread edits.
        uintptr_t g_dumpedFor = 0;
        void PollViews()
        {
            const uintptr_t win = GameMainWindow();
            if (!win) return;
            for (size_t i = 0; i < sizeof kViews / sizeof kViews[0]; ++i)
            {
                const uintptr_t panel = FindPanelGuarded(win, kViews[i]);
                int st = -2;
                uint8_t b = 0;
                if (panel && mem::Read8(panel + 0xB0, &b)) st = b;
                const bool open = st >= 0 && (st & 0x60) == 0x40;
                if (i == 0) g_warehouseOpen = open;
                if (st != g_viewLast[i])
                {
                    LOG("[view] %-24s panel %llX state %s0x%02X%s", kViews[i], U(panel), st < 0 ? "none " : "", st < 0 ? 0 : st,
                        open ? " OPEN" : "");
                    g_viewLast[i] = st;
                    if (i == 0 && open)
                    {
                        const uintptr_t c = g_warehouse.load();
                        if (c && c != g_dumpedFor) { DumpController(c); g_dumpedFor = c; }
                        g_bucketFull = true;   // the poller owns the bucket snapshot
                    }
                }
            }
        }

        // ------------------------------------------------------------ key swallowing
        // The game reads the keyboard from window messages (R3E 5.1), so the probe's
        // own chords are removed there. A key-up follows its swallowed key-down.
        HWND    g_hwnd = nullptr;
        WNDPROC g_oldProc = nullptr;
        bool    g_unicode = true;
        bool    g_swallowed[256] = {};

        bool Swallow(UINT vk, bool ctrl)
        {
            if (vk == VK_PAUSE) return true;
            return ctrl && ((vk >= VK_F1 && vk <= VK_F24) || vk == VK_END || vk == VK_DELETE);
        }

        LRESULT CALLBACK ProbeWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
        {
            if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_KEYUP || m == WM_SYSKEYUP)
            {
                const UINT vk = static_cast<UINT>(w & 0xFF);
                const bool down = m == WM_KEYDOWN || m == WM_SYSKEYDOWN;
                const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                if (down && Swallow(vk, ctrl)) { g_swallowed[vk] = true; return 0; }
                if (!down && g_swallowed[vk]) { g_swallowed[vk] = false; return 0; }
            }
            return g_unicode ? CallWindowProcW(g_oldProc, h, m, w, l) : CallWindowProcA(g_oldProc, h, m, w, l);
        }

        // The game's main window: class procedure +0x3E57F30 (R3E 5.1). The
        // first session subclassed the splash window by taking the first one found.
        BOOL CALLBACK FindGameWindow(HWND h, LPARAM out)
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != GetCurrentProcessId() || GetWindow(h, GW_OWNER)) return TRUE;
            if (static_cast<uintptr_t>(GetClassLongPtrW(h, GCLP_WNDPROC)) != Abs(kGameWndProc)) return TRUE;
            *reinterpret_cast<HWND*>(out) = h;
            return FALSE;
        }

        void SubclassGameWindow()
        {
            if (g_hwnd && IsWindow(g_hwnd)) return;
            g_hwnd = nullptr;
            HWND h = nullptr;
            EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&h));
            if (!h) return;
            g_unicode = IsWindowUnicode(h) != 0;
            const LONG_PTR prev = g_unicode ? SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ProbeWndProc))
                                            : SetWindowLongPtrA(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ProbeWndProc));
            g_hwnd = h;
            if (!prev) { LOG_ERR("[keys] subclassing window %p failed (%lu)", static_cast<void*>(h), GetLastError()); return; }
            g_oldProc = reinterpret_cast<WNDPROC>(prev);
            char cls[64] = {};
            GetClassNameA(h, cls, sizeof cls);
            LOG("[keys] game window %p class \"%s\" subclassed (%s, previous proc %llX)", static_cast<void*>(h), cls,
                g_unicode ? "unicode" : "ansi", U(reinterpret_cast<uintptr_t>(g_oldProc)));
        }

        void RestoreGameWindow()
        {
            if (!g_hwnd || !g_oldProc || !IsWindow(g_hwnd)) return;
            const LONG_PTR cur = g_unicode ? GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
            if (cur != reinterpret_cast<LONG_PTR>(ProbeWndProc)) return;   // someone chained after us; leave it
            if (g_unicode) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_oldProc));
            else SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_oldProc));
        }

        DWORD WINAPI Poller(LPVOID)
        {
            bool keyWas[16] = {};   // [0..8] chest keys, [9] Delete, [10] End, [11] Pause
            DWORD nextBucketPoll = 0;
            while (!g_stop.load())
            {
                Sleep(50);
                SubclassGameWindow();
                const bool warehouseOpen = g_warehouseOpen.load();

                const DWORD now = GetTickCount();
                if (g_bucketFull.exchange(false)) DumpBuckets(false);
                const DWORD diffAt = g_bucketDiffAt.load();
                if ((diffAt && now >= diffAt) || (warehouseOpen && now >= nextBucketPoll))
                {
                    if (diffAt && now >= diffAt) g_bucketDiffAt = 0;
                    DumpBuckets(true);
                    nextBucketPoll = now + 500;
                }

                const bool front = GameInFront();
                const bool ctrl = front && Down(VK_CONTROL) && !Down(VK_MENU);
                for (int k = 0; k < kChestCount; ++k)
                {
                    const bool d = ctrl && Down(kChestKeys[k]);
                    if (d && !keyWas[k])
                    {
                        LOG("[probe] Ctrl+F%d: %s", kChestKeys[k] - VK_F1 + 1, kChests[k].label);
                        g_pending = k;
                    }
                    keyWas[k] = d;
                }
                const bool panic = ctrl && Down(VK_DELETE);
                if (panic && !keyWas[9]) { LOG("[probe] Ctrl+Delete: panic close"); g_pending = kActPanic; }
                keyWas[9] = panic;
                const bool fid = ctrl && Down(VK_END);
                if (fid && !keyWas[10])
                {
                    g_fidelity = !g_fidelity.load();
                    LOG("[probe] Ctrl+End: 0x12, 0x14 and 0x0D packets %s", g_fidelity.load() ? "on" : "off");
                }
                keyWas[10] = fid;

                const bool pause = front && Down(VK_PAUSE);
                if (pause && !keyWas[11])
                {
                    LOG("[probe] Pause");
                    DumpInventoryInfo();
                    DumpBuckets(false);
                    DumpItemCategories();
                    DumpBucketItems("Kuku");
                    DumpBucketItems("Character");
                    if (const uintptr_t c = g_warehouse.load()) { ControllerLine("Pause", c); DumpController(c); }
                    if (const uintptr_t mo = g_modeObj.load())
                    {
                        char why[128];
                        GateOpen(mo, why, sizeof why);
                        LOG("[mode] Pause %llX: %s; remote %s stage %llu", U(mo), why, g_remoteOpen.load() ? "open" : "closed", g_remoteId.load());
                    }
                    LOG("[probe] GameMain window %llX, event wrap %llX", U(GameMainWindow()), U(EventWrap()));
                }
                keyWas[11] = pause;
            }
            return 0;
        }

        bool Hook(int idx, void* detour, void** orig)
        {
            const Target& t = kTargets[idx];
            char why[128];
            if (!psm::farhook::Install(t.name, Abs(t.rva), detour, orig, why, sizeof why))
            {
                LOG_ERR("[probe] hook %s at +%X failed: %s", t.name, t.rva, why);
                return false;
            }
            LOG_OK("[probe] hooked %s at +%X", t.name, t.rva);
            return true;
        }
    }

    bool Start()
    {
        const mem::Module& m = mem::Game();
        if (m.size != kSizeOfImage)
        {
            LOG_ERR("[probe] image size 0x%zX is not 1.0.0.2850's 0x%X. The probe is pinned to that build and does nothing here.",
                m.size, kSizeOfImage);
            return false;
        }
        int bad = 0;
        for (const Target& t : kTargets)
        {
            uint8_t b[16] = {};
            if (!mem::ReadBytes(Abs(t.rva), b, sizeof b) || memcmp(b, t.bytes, sizeof b) != 0)
            {
                LOG_ERR("[probe] %s at +%X does not hold the expected bytes (%02X %02X %02X %02X ...)", t.name, t.rva, b[0], b[1], b[2], b[3]);
                ++bad;
            }
        }
        if (bad)
        {
            LOG_ERR("[probe] %d target(s) differ. Nothing is hooked. Remove other storage mods and try again.", bad);
            return false;
        }
        g_view = MakeSs("WareHouseView");
        g_selector = MakeSs("selector-stagechart-self");
        if (!g_view || !g_selector) { LOG_ERR("[probe] out of memory"); return false; }

        const bool ok = Hook(kHandler, hkHandler, &oHandler) &&
                        Hook(kModeSwitch, hkModeSwitch, &oModeSwitch) &&
                        Hook(kWarehouseClose, hkWarehouseClose, &oWarehouseClose) &&
                        Hook(kStageClose, hkStageClose, &oStageClose);
        if (!ok)
        {
            // The open path must not run without the close hook and the main-thread tick.
            LOG_ERR("[probe] a required hook failed; removing all hooks");
            psm::farhook::RemoveAll();
            return false;
        }
        Hook(kSetInventory, hkSetInventory, &oSetInventory);
        Hook(kMenuRequest, hkMenuRequest, &oMenuRequest);
        Hook(kItemDetail, hkItemDetail, &oItemDetail);
        Hook(kCounting, hkCounting, &oCounting);
        Hook(kStageChartListener, hkListener, &oListener);
        Hook(kMoveCheck, hkMoveCheck, &oMoveCheck);
        Hook(kMoveSend, hkMoveSend, &oMoveSend);
        Hook(kInputBlockSet, hkInputBlock, &oInputBlock);
        Hook(kMoveDialogConfirm, hkMoveDialogConfirm, &oMoveDialogConfirm);
        g_poller = CreateThread(nullptr, 0, Poller, nullptr, 0, nullptr);
        LOG("[probe] probe 2h running. Ctrl+F1..F8 open or close Private Storage, Gatherables, Dresser, Refrigerator, Collecting, "
            "Camp Straw, Bird Feed, Town Warehouse; Ctrl+F9 Kuku Pot; "
            "Ctrl+Delete panic close; Ctrl+End toggles the 0x12/0x14/0x0D packets (on); Pause dumps state.");
        return true;
    }

    void Stop()
    {
        g_stop = true;
        RestoreGameWindow();
        psm::farhook::RemoveAll();
    }
}

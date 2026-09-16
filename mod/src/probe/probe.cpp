#include "probe/probe.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "probe/targets_2850.h"

namespace psm::probe
{
    using namespace psm::probe2850;

    namespace
    {
        std::atomic<bool> g_stop{false};
        HANDLE g_poller = nullptr;

        uintptr_t Base() { return mem::Game().base; }
        uintptr_t Abs(uint32_t rva) { return Base() + rva; }

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
                w += snprintf(out + w, cap - w, " %llX", static_cast<unsigned long long>(f - Base()));
                ++shown;
            }
        }

        // A printable C string at p, or at *p. Engine strings are an object
        // whose first qword is the char*, so one level of indirection covers
        // both shapes seen in the handler.
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

        // Budget so a detour on a busy path cannot bury the file.
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

        // ------------------------------------------------------------ captured
        std::atomic<uintptr_t> g_warehouse{0};   // UIGamePlayControlRootWarehouse2 instance
        std::atomic<uintptr_t> g_modeObj{0};
        std::atomic<uintptr_t> g_pm{0};

        void* oHandler = nullptr, *oSetInventory = nullptr, *oMenuRequest = nullptr, *oModeSwitch = nullptr,
            *oFindPanelTop = nullptr, *oCanShow = nullptr, *oItemDetail = nullptr, *oCounting = nullptr;

        // ------------------------------------------------------------ detours
        using Fn4 = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);

        void DumpPacket(uintptr_t pkt)
        {
            uint8_t kind = 0;
            if (!mem::Read8(pkt, &kind)) { LOG("[handler]   packet %llX unreadable", static_cast<unsigned long long>(pkt)); return; }
            uintptr_t subs = 0; uint32_t n = 0;
            mem::ReadPtr(pkt + 8, &subs);
            mem::Read32(pkt + 0x10, &n);
            LOG("[handler]   packet kind 0x%02X, %u sub-commands at %llX", kind, n, static_cast<unsigned long long>(subs));
            if (!subs || n > 64) return;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uintptr_t s = subs + 0x18ull * i;
                uint8_t sk = 0; uintptr_t args = 0; uint32_t an = 0;
                mem::Read8(s, &sk);
                mem::ReadPtr(s + 8, &args);
                mem::Read32(s + 0x10, &an);
                uint64_t raw[3] = {};
                mem::ReadBytes(s, raw, sizeof raw);
                LOG("[handler]   sub %u kind 0x%02X, %u args at %llX (raw %016llX %016llX %016llX)", i, sk, an,
                    static_cast<unsigned long long>(args), raw[0], raw[1], raw[2]);
                if (!args || an > 32) continue;
                for (uint32_t k = 0; k < an; ++k)
                {
                    const uintptr_t a = args + 0x10ull * k;
                    uint64_t q[2] = {};
                    mem::ReadBytes(a, q, sizeof q);
                    char t0[160], t1[160];
                    TryText(static_cast<uintptr_t>(q[0]), t0, sizeof t0);
                    TryText(static_cast<uintptr_t>(q[1]), t1, sizeof t1);
                    // Also as an inline string at +8, which is the other shape the
                    // argument walk in the handler allows.
                    char t2[160] = {};
                    if (!t1[0]) TryText(a + 8, t2, sizeof t2);
                    LOG("[handler]     arg %u %016llX %016llX  \"%s\" \"%s\"%s%s", k, q[0], q[1], t0, t1[0] ? t1 : t2,
                        t2[0] ? " (inline)" : "", "");
                }
            }
        }

        uintptr_t __fastcall hkHandler(uintptr_t self, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
        {
            static Budget b;
            if (b.Take(40))
            {
                uintptr_t vt = 0;
                mem::ReadPtr(self, &vt);
                if (vt == Abs(kWarehouseVtable)) g_warehouse = self;
                char st[256]; StackLine(st, sizeof st);
                LOG("[handler] this %llX (vtable +%llX) rdx %llX r8 %llX r9 %llX | %s", static_cast<unsigned long long>(self),
                    static_cast<unsigned long long>(mem::Rva(vt)), static_cast<unsigned long long>(rdx),
                    static_cast<unsigned long long>(r8), static_cast<unsigned long long>(r9), st);
                if (r9) DumpPacket(r9);
            }
            return static_cast<Fn4>(oHandler)(self, rdx, r8, r9);
        }

        uintptr_t __fastcall hkSetInventory(uintptr_t self, uintptr_t text, uintptr_t r8, uintptr_t r9)
        {
            char s[256] = {};
            mem::ReadCString(text, s, sizeof s);
            char st[256]; StackLine(st, sizeof st);
            LOG("[setinv] this %llX \"%s\" | %s", static_cast<unsigned long long>(self), s, st);
            return static_cast<Fn4>(oSetInventory)(self, text, r8, r9);
        }

        uintptr_t __fastcall hkMenuRequest(uintptr_t obj, uintptr_t a, uintptr_t b, uintptr_t flag)
        {
            char sa[160] = {}, sb[160] = {};
            mem::ReadCString(a, sa, sizeof sa);
            mem::ReadCString(b, sb, sizeof sb);
            char st[256]; StackLine(st, sizeof st);
            LOG("[menureq] obj %llX \"%s\" \"%s\" flag %u | %s", static_cast<unsigned long long>(obj), sa, sb,
                static_cast<unsigned>(flag & 0xFF), st);
            return static_cast<Fn4>(oMenuRequest)(obj, a, b, flag);
        }

        // Called every frame. Logs the mode cluster only when it changes.
        uint8_t g_modePrev[0x40] = {};
        bool    g_modeHave = false;
        uintptr_t __fastcall hkModeSwitch(uintptr_t obj, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
        {
            uint8_t now[0x40] = {};
            if (mem::ReadBytes(obj + 0x20, now, sizeof now))
            {
                if (g_modeObj.load() != obj)
                {
                    LOG("[mode] mode object %llX", static_cast<unsigned long long>(obj));
                    g_modeObj = obj;
                    g_modeHave = false;
                }
                if (!g_modeHave || memcmp(now, g_modePrev, sizeof now) != 0)
                {
                    static Budget bud;
                    if (bud.Take(20))
                    {
                        char hex[0x40 * 3 + 1] = {};
                        for (int i = 0; i < 0x40; ++i) snprintf(hex + i * 3, 4, "%02X ", now[i]);
                        LOG("[mode] +20: %s", hex);
                    }
                    memcpy(g_modePrev, now, sizeof now);
                    g_modeHave = true;
                }
            }
            return static_cast<Fn4>(oModeSwitch)(obj, rdx, r8, r9);
        }

        // Records the panel manager and the first few hundred distinct names asked for.
        uintptr_t __fastcall hkFindPanelTop(uintptr_t pm, uintptr_t name, uintptr_t r8, uintptr_t r9)
        {
            if (!g_pm.load())
            {
                uintptr_t arr = 0; uint32_t n = 0;
                if (mem::ReadPtr(pm + 0x30EB8, &arr) && mem::Read32(pm + 0x30EC0, &n) && n && n < 0x1000)
                {
                    g_pm = pm;
                    LOG("[panels] panel manager %llX, %u panels", static_cast<unsigned long long>(pm), n);
                }
            }
            return static_cast<Fn4>(oFindPanelTop)(pm, name, r8, r9);
        }

        // Shared by 131 classes; only the warehouse controller is reported, and only when the answer flips.
        std::atomic<int> g_canShowLast{-1};
        uintptr_t __fastcall hkCanShow(uintptr_t obj, uintptr_t rdx, uintptr_t r8, uintptr_t r9)
        {
            const uintptr_t r = static_cast<Fn4>(oCanShow)(obj, rdx, r8, r9);
            uintptr_t vt = 0;
            if (mem::ReadPtr(obj, &vt) && vt == Abs(kWarehouseVtable))
            {
                if (g_warehouse.load() != obj)
                {
                    g_warehouse = obj;
                    LOG("[canshow] warehouse controller %llX", static_cast<unsigned long long>(obj));
                }
                const int v = static_cast<int>(r & 0xFF);
                if (g_canShowLast.exchange(v) != v)
                {
                    char st[256]; StackLine(st, sizeof st);
                    LOG("[canshow] warehouse %llX -> %d | %s", static_cast<unsigned long long>(obj), v, st);
                }
            }
            return r;
        }

        uintptr_t __fastcall hkItemDetail(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
        {
            char st[256]; StackLine(st, sizeof st);
            LOG("[modal] item detail opener (%llX %llX) | %s", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), st);
            return static_cast<Fn4>(oItemDetail)(a, b, c, d);
        }
        uintptr_t __fastcall hkCounting(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
        {
            char st[256]; StackLine(st, sizeof st);
            LOG("[modal] counting opener (%llX %llX) | %s", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), st);
            return static_cast<Fn4>(oCounting)(a, b, c, d);
        }

        // ------------------------------------------------------------ poller
        using FindPanelFn = uintptr_t(__fastcall*)(uintptr_t, const char*);

        uintptr_t FindPanelGuarded(uintptr_t pm, const char* name)
        {
            __try { return static_cast<FindPanelFn>(oFindPanelTop)(pm, name); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        }

        const char* const kViews[] = {
            "WareHouseView", "MainMenuView2", "WorldMapView", "ModalMessageView", "HousingManagementPanel",
            "KeyGuidePanel", "ItemGiftInventory", "NpcStoreBuyView", "NpcStoreSellView",
        };
        int g_viewLast[sizeof kViews / sizeof kViews[0]];

        void DumpController(uintptr_t c)
        {
            LOG("[ctrl] warehouse controller %llX, non-zero qwords up to +0x480:", static_cast<unsigned long long>(c));
            char line[900]; int w = 0;
            for (unsigned off = 0; off < 0x480; off += 8)
            {
                uint64_t q = 0;
                if (!mem::Read64(c + off, &q) || !q) continue;
                w += snprintf(line + w, sizeof line - w, " +%X=%llX", off, q);
                if (w > 760) { LOG("[ctrl] %s", line); w = 0; }
            }
            if (w) LOG("[ctrl] %s", line);
        }

        void DumpInventoryInfo()
        {
            uintptr_t mgr = 0;
            if (!mem::ReadPtr(Abs(kInvMgrGlobal), &mgr)) { LOG("[inv] manager not created (global +%X)", kInvMgrGlobal); return; }
            uint32_t n = 0; uintptr_t tbl = 0, arr = 0;
            mem::Read32(mgr + 0x08, &n);
            mem::ReadPtr(mgr + 0x28, &tbl);
            mem::ReadPtr(mgr + 0x58, &arr);
            LOG("[inv] manager %llX count %u index %llX records %llX", static_cast<unsigned long long>(mgr), n,
                static_cast<unsigned long long>(tbl), static_cast<unsigned long long>(arr));
            if (!arr || n > 256) return;
            for (uint32_t i = 0; i < n; ++i)
            {
                uintptr_t info = 0;
                mem::ReadPtr(arr + 8ull * i, &info);
                uint32_t fileId = 0, bodyOff = 0;
                if (tbl) { mem::Read32(tbl + 8ull * i, &fileId); mem::Read32(tbl + 8ull * i + 4, &bodyOff); }
                uint16_t base = 0, max = 0;
                char name[96] = {};
                if (info)
                {
                    mem::Read16(info + 0x48, &base);
                    mem::Read16(info + 0x4A, &max);
                    // Find the record's name among its first qwords, since where it is kept is not known yet.
                    for (unsigned o = 0; o < 0x48 && !name[0]; o += 8)
                    {
                        uintptr_t p = 0;
                        if (mem::ReadPtr(info + o, &p)) TryText(p, name, sizeof name);
                        if (name[0] && !isalpha(static_cast<unsigned char>(name[0]))) name[0] = 0;
                    }
                }
                LOG("[inv]   [%2u] file id %u body +%X record %llX slots %u max %u \"%s\"", i, fileId, bodyOff,
                    static_cast<unsigned long long>(info), base, max, name);
            }
            uintptr_t first = 0;
            if (mem::ReadPtr(arr, &first))
            {
                char line[900]; int w = 0;
                for (unsigned off = 0; off < 0x60; off += 8)
                {
                    uint64_t q = 0; mem::Read64(first + off, &q);
                    w += snprintf(line + w, sizeof line - w, " +%X=%llX", off, q);
                }
                LOG("[inv] first record raw:%s", line);
            }
        }

        DWORD WINAPI Poller(LPVOID)
        {
            for (auto& v : g_viewLast) v = -1;
            bool f10 = false;
            uintptr_t dumpedFor = 0;
            while (!g_stop.load())
            {
                Sleep(100);
                const uintptr_t pm = g_pm.load();
                if (pm)
                {
                    for (size_t i = 0; i < sizeof kViews / sizeof kViews[0]; ++i)
                    {
                        const uintptr_t panel = FindPanelGuarded(pm, kViews[i]);
                        int st = -2;
                        uint8_t b = 0;
                        if (panel && mem::Read8(panel + 0xB0, &b)) st = b;
                        if (st != g_viewLast[i])
                        {
                            LOG("[view] %-24s panel %llX state %s0x%02X%s", kViews[i], static_cast<unsigned long long>(panel),
                                st < 0 ? "none " : "", st < 0 ? 0 : st, (st >= 0 && (st & 0x60) == 0x40) ? " OPEN" : "");
                            g_viewLast[i] = st;
                            if (i == 0 && st >= 0 && (st & 0x60) == 0x40)
                            {
                                const uintptr_t c = g_warehouse.load();
                                if (c && c != dumpedFor) { DumpController(c); dumpedFor = c; }
                            }
                        }
                    }
                }
                const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
                if (down && !f10)
                {
                    LOG("[probe] F10");
                    DumpInventoryInfo();
                    if (const uintptr_t c = g_warehouse.load()) DumpController(c);
                    if (const uintptr_t mo = g_modeObj.load())
                    {
                        uint8_t m[0x40] = {};
                        mem::ReadBytes(mo + 0x20, m, sizeof m);
                        char hex[0x40 * 3 + 1] = {};
                        for (int i = 0; i < 0x40; ++i) snprintf(hex + i * 3, 4, "%02X ", m[i]);
                        LOG("[mode] F10 %llX +20: %s", static_cast<unsigned long long>(mo), hex);
                    }
                    uintptr_t root = 0, menu = 0;
                    if (mem::ReadPtr(Abs(kMenuRootGlobal), &root)) mem::ReadPtr(root + 0x98, &menu);
                    LOG("[menu] root %llX request object %llX", static_cast<unsigned long long>(root), static_cast<unsigned long long>(menu));
                }
                f10 = down;
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
                // Another plugin may already have its jump here.
                LOG_ERR("[probe] %s at +%X does not hold the expected bytes (%02X %02X %02X %02X ...)", t.name, t.rva, b[0], b[1], b[2], b[3]);
                ++bad;
            }
        }
        if (bad)
        {
            LOG_ERR("[probe] %d target(s) differ. Nothing is hooked. Remove other storage mods and try again.", bad);
            return false;
        }
        Hook(kHandler, hkHandler, &oHandler);
        Hook(kSetInventory, hkSetInventory, &oSetInventory);
        Hook(kMenuRequest, hkMenuRequest, &oMenuRequest);
        Hook(kModeSwitch, hkModeSwitch, &oModeSwitch);
        Hook(kFindPanelTop, hkFindPanelTop, &oFindPanelTop);
        Hook(kCanShow, hkCanShow, &oCanShow);
        Hook(kItemDetail, hkItemDetail, &oItemDetail);
        Hook(kCounting, hkCounting, &oCounting);
        g_poller = CreateThread(nullptr, 0, Poller, nullptr, 0, nullptr);
        LOG("[probe] running. Open the camp storage and each housing chest in person, open an item's details and "
            "a quantity dialog, close with Esc, and press F10 once with a chest open and once in the world.");
        return true;
    }

    void Stop()
    {
        g_stop = true;
        psm::farhook::RemoveAll();
    }
}

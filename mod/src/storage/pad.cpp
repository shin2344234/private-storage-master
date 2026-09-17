#include "storage/pad.h"

#include <Windows.h>
#include <Xinput.h>
#include <atomic>
#include <cstring>

#include "core/log.h"
#include "core/settings.h"
#include "game/mem.h"

namespace psm::pad
{
    namespace
    {
        using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        GetStateFn g_get = nullptr;        // our own XInput, never the game's import
        GetStateFn g_gameGet = nullptr;    // what the game's import slot held
        uintptr_t* g_slot = nullptr;       // the game's import slot, patched

        constexpr uint16_t kB = XINPUT_GAMEPAD_B;
        std::atomic<bool> g_hideB{false};
        std::atomic<uint16_t> g_hiding{0};   // pressed buttons of combos in progress, hidden until released

        std::atomic<uint16_t> g_last{0};
        bool g_present[XUSER_MAX_COUNT] = {};
        DWORD g_lastHunt = 0;

        uint16_t HideMask(uint16_t buttons)
        {
            const Settings::Values& v = Settings::Get();
            uint16_t hiding = static_cast<uint16_t>(g_hiding.load() & buttons);   // keep hiding until the button is up
            for (int i = 0; i < Settings::kStorages; ++i)
            {
                const Settings::PadBind& p = v.pad[i];
                if (p.press && (buttons & p.press) && (buttons & p.hold) == p.hold) hiding |= p.press;
            }
            g_hiding.store(hiding);
            uint16_t mask = hiding;
            if (g_hideB.load())
            {
                if (buttons & kB) mask |= kB;
                else g_hideB = false;
            }
            return mask;
        }

        DWORD WINAPI FilteredGetState(DWORD user, XINPUT_STATE* st)
        {
            const DWORD r = g_gameGet(user, st);
            if (r == ERROR_SUCCESS && st)
            {
                const uint16_t mask = HideMask(st->Gamepad.wButtons);
                st->Gamepad.wButtons = static_cast<uint16_t>(st->Gamepad.wButtons & ~mask);
            }
            return r;
        }

        // The game imports XInputGetState by ordinal 2 from XINPUT1_4.dll (R3E 5.2).
        uintptr_t* FindImportSlot()
        {
            const uintptr_t base = mem::Game().base;
            const auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) return nullptr;
            for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d)
            {
                const char* dll = reinterpret_cast<const char*>(base + d->Name);
                // Any loaded library that exports XInputGetState is an XInput, whatever its version.
                HMODULE lib = GetModuleHandleA(dll);
                if (!lib || !GetProcAddress(lib, "XInputGetState")) continue;
                auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
                auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++slots)
                {
                    bool match = false;
                    if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) match = IMAGE_ORDINAL64(names->u1.Ordinal) == 2;
                    else
                    {
                        const auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                        match = strcmp(reinterpret_cast<const char*>(byName->Name), "XInputGetState") == 0;
                    }
                    if (match) return reinterpret_cast<uintptr_t*>(&slots->u1.Function);
                }
            }
            return nullptr;
        }

        bool WriteSlot(uintptr_t* slot, uintptr_t value)
        {
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return false;
            *slot = value;
            VirtualProtect(slot, sizeof *slot, old, &old);
            return true;
        }
    }

    bool Init()
    {
        for (const wchar_t* n : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
        {
            if (HMODULE h = LoadLibraryW(n))
            {
                g_get = reinterpret_cast<GetStateFn>(GetProcAddress(h, "XInputGetState"));
                if (g_get) break;
            }
        }
        if (!g_get)
        {
            LOG_ERR("[pad] no XInput library, so controller combos do not work");
            return false;
        }
        bool anyCombo = false;
        for (int i = 0; i < Settings::kStorages; ++i) anyCombo |= Settings::Get().pad[i].press != 0;
        if (!anyCombo) return true;

        g_slot = FindImportSlot();
        uintptr_t cur = 0;
        if (!g_slot || !mem::Read64(reinterpret_cast<uintptr_t>(g_slot), &cur) || !cur)
        {
            LOG_ERR("[pad] the game's XInputGetState import was not found; combos work but the game also sees them");
            g_slot = nullptr;
            return true;
        }
        g_gameGet = reinterpret_cast<GetStateFn>(cur);
        if (!WriteSlot(g_slot, reinterpret_cast<uintptr_t>(&FilteredGetState)))
        {
            LOG_ERR("[pad] could not patch the game's XInputGetState import (%lu); combos work but the game also sees them", GetLastError());
            g_slot = nullptr;
            return true;
        }
        LOG("[pad] filtering the game's XInputGetState import at %p", static_cast<void*>(g_slot));
        return true;
    }

    uint16_t Poll()
    {
        if (!g_get) return 0;
        const DWORD now = GetTickCount();
        // Asking an empty slot is the slow XInput call, so empty slots are asked once a second.
        const bool hunt = now - g_lastHunt >= 1000;
        if (hunt) g_lastHunt = now;
        uint16_t buttons = 0;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
        {
            if (!g_present[i] && !hunt) continue;
            XINPUT_STATE st{};
            const bool ok = g_get(i, &st) == ERROR_SUCCESS;
            if (ok != g_present[i]) LOG("[pad] controller %s XInput slot %lu", ok ? "found on" : "gone from", i);
            g_present[i] = ok;
            if (ok) buttons |= st.Gamepad.wButtons;
        }
        g_last = buttons;
        return buttons;
    }

    uint16_t Last() { return g_last.load(); }

    int Slot()
    {
        for (int i = 0; i < XUSER_MAX_COUNT; ++i)
            if (g_present[i]) return i;
        return -1;
    }

    void HideBUntilReleased() { g_hideB = true; }

    void Shutdown()
    {
        if (!g_slot || !g_gameGet) return;
        uintptr_t cur = 0;
        if (mem::Read64(reinterpret_cast<uintptr_t>(g_slot), &cur) && cur == reinterpret_cast<uintptr_t>(&FilteredGetState))
            WriteSlot(g_slot, reinterpret_cast<uintptr_t>(g_gameGet));
        g_slot = nullptr;
    }
}

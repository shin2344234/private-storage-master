#include "core/settings.h"

#include <Windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "core/paths.h"
#include "version.h"

namespace psm::Settings
{
    namespace
    {
        Values g_values;

        struct StorageInfo
        {
            const char* key;        // ini prefix
            const char* oldKey;     // PrivateStorageAnywhere.ini prefix, or nullptr
            const char* comment;    // what it is, for the written ini
            const char* defKey;
            const char* defPad;
        };
        const StorageInfo kInfo[kStorages] = {
            {"PrivateStorage", "Private", "Private Storage, the camp storage box", "Ctrl+F1", "LB+LS"},
            {"Gatherables", "Gatherables", "Gatherables Chest (housing)", "Ctrl+F2", "LB+RS"},
            {"Dresser", "Dresser", "Wardrobe (housing)", "Ctrl+F3", ""},
            {"Refrigerator", "Refrigerator", "Kuku Cooler (housing)", "Ctrl+F4", ""},
            {"Collecting", "Collecting", "Collectibles Chest (housing)", "Ctrl+F5", ""},
            {"CampStraw", nullptr, "Camp Straw, the camp feed bin", "Ctrl+F6", ""},
            {"BirdFeed", nullptr, "Bird Feed, the camp bird feeder", "Ctrl+F7", ""},
            {"TownWarehouse", nullptr, "Town Warehouse, the packaged trade goods kept at warehouse NPCs", "Ctrl+F8", ""},
            {"AbyssGear", nullptr, "Abyss gear storage, the Kuku Pot bag", "Ctrl+F9", ""},
        };

        void Trim(char* s)
        {
            size_t n = strlen(s);
            while (n && isspace(static_cast<unsigned char>(s[n - 1]))) s[--n] = 0;
            size_t i = 0;
            while (s[i] && isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i) memmove(s, s + i, n - i + 1);
        }

        struct NamedKey { const char* name; uint8_t vk; };
        const NamedKey kKeys[] = {
            {"Insert", VK_INSERT}, {"Ins", VK_INSERT}, {"Delete", VK_DELETE}, {"Del", VK_DELETE},
            {"Home", VK_HOME}, {"End", VK_END}, {"PageUp", VK_PRIOR}, {"PgUp", VK_PRIOR},
            {"PageDown", VK_NEXT}, {"PgDn", VK_NEXT}, {"Pause", VK_PAUSE}, {"ScrollLock", VK_SCROLL},
            {"Up", VK_UP}, {"Down", VK_DOWN}, {"Left", VK_LEFT}, {"Right", VK_RIGHT},
            {"Space", VK_SPACE}, {"Tab", VK_TAB}, {"Enter", VK_RETURN}, {"Backspace", VK_BACK},
            {"CapsLock", VK_CAPITAL}, {"NumLock", VK_NUMLOCK},
            {"Multiply", VK_MULTIPLY}, {"Add", VK_ADD}, {"Subtract", VK_SUBTRACT}, {"Decimal", VK_DECIMAL},
            {"Divide", VK_DIVIDE}, {"Semicolon", VK_OEM_1}, {"Equals", VK_OEM_PLUS}, {"Comma", VK_OEM_COMMA},
            {"Minus", VK_OEM_MINUS}, {"Period", VK_OEM_PERIOD}, {"Slash", VK_OEM_2}, {"Tilde", VK_OEM_3},
            {"LeftBracket", VK_OEM_4}, {"Backslash", VK_OEM_5}, {"RightBracket", VK_OEM_6}, {"Quote", VK_OEM_7},
        };

        struct NamedButton { const char* name; uint16_t bit; };
        const NamedButton kButtons[] = {
            {"Up", 0x0001}, {"Down", 0x0002}, {"Left", 0x0004}, {"Right", 0x0008},
            {"Start", 0x0010}, {"Menu", 0x0010}, {"Back", 0x0020}, {"View", 0x0020},
            {"LS", 0x0040}, {"RS", 0x0080}, {"LB", 0x0100}, {"RB", 0x0200},
            {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
        };

        bool IsNone(const char* s) { return !*s || _stricmp(s, "none") == 0 || strcmp(s, "0") == 0 || strcmp(s, "00") == 0; }

        // One key name, no modifiers. 0 when unknown.
        uint8_t KeyFromName(const char* s)
        {
            if ((s[0] == 'F' || s[0] == 'f') && isdigit(static_cast<unsigned char>(s[1])))
            {
                const int n = atoi(s + 1);
                if (n >= 1 && n <= 24) return static_cast<uint8_t>(VK_F1 + n - 1);
                return 0;
            }
            if (!s[1] && isalnum(static_cast<unsigned char>(s[0]))) return static_cast<uint8_t>(toupper(static_cast<unsigned char>(s[0])));
            if (_strnicmp(s, "Numpad", 6) == 0 && isdigit(static_cast<unsigned char>(s[6])) && !s[7]) return static_cast<uint8_t>(VK_NUMPAD0 + s[6] - '0');
            if (_strnicmp(s, "Num", 3) == 0 && isdigit(static_cast<unsigned char>(s[3])) && !s[4]) return static_cast<uint8_t>(VK_NUMPAD0 + s[3] - '0');
            for (const NamedKey& k : kKeys)
                if (_stricmp(s, k.name) == 0) return k.vk;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            {
                const unsigned long v = strtoul(s + 2, nullptr, 16);
                if (v > 0 && v < 256) return static_cast<uint8_t>(v);
            }
            return 0;
        }

        // Mouse buttons and modifier keys cannot be a binding's key.
        bool Bindable(uint8_t vk)
        {
            if (vk <= VK_XBUTTON2 || vk == VK_ESCAPE) return false;
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) return false;
            if (vk >= VK_LSHIFT && vk <= VK_RMENU) return false;
            if (vk == VK_LWIN || vk == VK_RWIN) return false;
            return true;
        }

        bool ParseKey(const char* text, KeyBind& out)
        {
            out = KeyBind{};
            if (IsNone(text)) return true;
            char buf[64];
            strncpy_s(buf, text, _TRUNCATE);
            uint8_t mods = 0, vk = 0;
            char* ctx = nullptr;
            for (char* tok = strtok_s(buf, "+", &ctx); tok; tok = strtok_s(nullptr, "+", &ctx))
            {
                Trim(tok);
                if (vk) return false;   // a key has to be last
                if (_stricmp(tok, "Ctrl") == 0 || _stricmp(tok, "Control") == 0) mods |= kModCtrl;
                else if (_stricmp(tok, "Shift") == 0) mods |= kModShift;
                else if (_stricmp(tok, "Alt") == 0) mods |= kModAlt;
                else if (!(vk = KeyFromName(tok))) return false;
            }
            if (!vk || !Bindable(vk)) return false;
            out.vk = vk;
            out.mods = mods;
            return true;
        }

        uint16_t ButtonFromName(const char* s)
        {
            for (const NamedButton& b : kButtons)
                if (_stricmp(s, b.name) == 0) return b.bit;
            return 0;
        }

        bool ParsePad(const char* text, PadBind& out)
        {
            out = PadBind{};
            if (IsNone(text)) return true;
            char buf[96];
            strncpy_s(buf, text, _TRUNCATE);
            uint16_t hold = 0, last = 0;
            char* ctx = nullptr;
            for (char* tok = strtok_s(buf, "+", &ctx); tok; tok = strtok_s(nullptr, "+", &ctx))
            {
                Trim(tok);
                const uint16_t bit = ButtonFromName(tok);
                if (!bit) return false;
                hold |= last;
                last = bit;
            }
            if (!last || (hold & last)) return false;
            out.hold = hold;
            out.press = last;
            return true;
        }

        // ---------------------------------------------------------------- import
        uint8_t Hex8(const char* s) { return static_cast<uint8_t>(strtoul(s, nullptr, 16) & 0xFF); }

        void ImportOld(const std::wstring& path)
        {
            char v[64];
            const auto get = [&](const char* key, const char* def) {
                GetPrivateProfileStringA("Settings", key, def, v, sizeof v, Paths::FileUtf8(L"PrivateStorageAnywhere.ini").c_str());
                Trim(v);
                return v;
            };
            (void)path;
            g_values.enabled = atoi(get("Enabled", "1")) != 0;
            g_values.privateStorageSlots = atoi(get("PrivateStorageSlots", "0"));
            g_values.privateStorageExpansions = atoi(get("PrivateStorageExpansions", "-1"));

            for (int i = 0; i < kStorages; ++i)
            {
                const StorageInfo& s = kInfo[i];
                if (!s.oldKey) continue;
                char name[64];

                snprintf(name, sizeof name, "%sHotkey", s.oldKey);
                const uint8_t vk = Hex8(get(name, "00"));
                snprintf(name, sizeof name, "%sModifier", s.oldKey);
                const uint8_t mod = Hex8(get(name, "00"));
                KeyBind kb{};
                if (vk && Bindable(vk))
                {
                    kb.vk = vk;
                    if (mod == VK_CONTROL || mod == VK_LCONTROL || mod == VK_RCONTROL) kb.mods = kModCtrl;
                    else if (mod == VK_SHIFT || mod == VK_LSHIFT || mod == VK_RSHIFT) kb.mods = kModShift;
                    else if (mod == VK_MENU || mod == VK_LMENU || mod == VK_RMENU) kb.mods = kModAlt;
                    else if (mod)
                        LOG_NOTE("[settings] import: %sModifier=%02X is not Ctrl, Shift or Alt, so %s uses no modifier", s.oldKey, mod, s.key);
                }
                else if (vk)
                    LOG_NOTE("[settings] import: %sHotkey=%02X is a mouse or modifier key, which this mod does not bind; %s keeps no key",
                             s.oldKey, vk, s.key);
                g_values.key[i] = kb;

                snprintf(name, sizeof name, "%sControllerButton", s.oldKey);
                const uint16_t button = static_cast<uint16_t>(strtoul(get(name, "0000"), nullptr, 16));
                snprintf(name, sizeof name, "%sControllerModifier", s.oldKey);
                const uint16_t modifier = static_cast<uint16_t>(strtoul(get(name, "0000"), nullptr, 16));
                PadBind pb{};
                // One button fires. The old mod took a mask; its lowest bit is kept.
                if (button)
                {
                    pb.press = static_cast<uint16_t>(button & (~button + 1));
                    pb.hold = static_cast<uint16_t>(modifier & ~pb.press);
                }
                g_values.pad[i] = pb;
            }
            char symbol[16];
            GetPrivateProfileStringA("Settings", "SymbolHotkey", "00", symbol, sizeof symbol, Paths::FileUtf8(L"PrivateStorageAnywhere.ini").c_str());
            if (Hex8(symbol))
                LOG_NOTE("[settings] import: Symbol storage has no binding here. The game has no way to put anything in it.");
            g_values.imported = true;
        }

        // ---------------------------------------------------------------- writing
        void WriteIni(const std::wstring& path)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f)
            {
                LOG_ERR("[settings] could not write %s", Paths::FileUtf8(PSM_INI).c_str());
                return;
            }
            const Values& v = g_values;
            char a[64], b[64];
            fprintf(f, "[PrivateStorageMaster]\n\n");
            if (v.imported)
                fprintf(f, "; Written from your PrivateStorageAnywhere.ini the first time this mod ran.\n; That file is left as it was and is not read again.\n\n");
            fprintf(f,
                    "; ------------------------------------------------------------------ general\n\n"
                    "; 1 turns the mod on. 0 loads it and does nothing.\n"
                    "Enabled=%d\n\n"
                    "; 1 writes everything the mod does to PrivateStorageMaster.log. 0 writes only\n"
                    "; the startup summary, errors and the capacity dump. Turn it on for a bug report.\n"
                    "DebugLog=%d\n\n",
                    v.enabled ? 1 : 0, v.debugLog ? 1 : 0);
            fprintf(f,
                    "; ------------------------------------------------------------------ keys\n\n"
                    "; Each storage has a key and a controller combo. Press it to open that storage\n"
                    "; from anywhere, press it again to close. Pressing another storage's key while\n"
                    "; one is open switches straight to that one.\n"
                    ";\n"
                    "; Keys are names with optional Ctrl, Shift or Alt: F4, Ctrl+F1, Shift+I. Names:\n"
                    "; F1 to F24, A to Z, 0 to 9, Numpad0 to Numpad9, Insert, Delete, Home, End,\n"
                    "; PageUp, PageDown, Pause, ScrollLock, Up, Down, Left, Right, Space, Tab, Enter,\n"
                    "; Backspace, Multiply, Add, Subtract, Decimal, Divide, Semicolon, Equals, Comma,\n"
                    "; Minus, Period, Slash, Tilde, LeftBracket, Backslash, RightBracket, Quote.\n"
                    "; None turns a key off. A bound key is hidden from the game while its modifiers\n"
                    "; are held, so Ctrl+F1 opens storage and does nothing else.\n"
                    ";\n"
                    "; Controller combos use Xbox button names joined with +. Every button but the\n"
                    "; last is held, the last is pressed: LB+LS is hold LB, click the left stick.\n"
                    "; Names: A, B, X, Y, LB, RB, LS, RS, Up, Down, Left, Right, Start, Back.\n"
                    "; The pressed button is hidden from the game while the others are held.\n"
                    "; A PlayStation pad works through Steam Input or DS4Windows. Holding a combo\n"
                    "; for most of a second closes the open storage.\n\n");
            for (int i = 0; i < kStorages; ++i)
            {
                fprintf(f, "; %s\n%sKey=%s\n%sPad=%s\n\n", kInfo[i].comment, kInfo[i].key, KeyText(v.key[i], a, sizeof a), kInfo[i].key,
                        PadText(v.pad[i], b, sizeof b));
            }
            fprintf(f,
                    "; Writes the size and contents count of every storage to the log.\n"
                    "CapacityDumpKey=%s\n\n",
                    KeyText(v.dumpKey, a, sizeof a));
            fprintf(f,
                    "; ------------------------------------------------------------------ capacity\n\n"
                    "; Capacity changes take effect the next time the game starts.\n\n"
                    "; 1 leaves every storage at the size the game (or another mod) gives it and\n"
                    "; ignores the two settings below. Use it with JSON capacity mods.\n"
                    "LeaveCapacityAlone=%d\n\n"
                    "; 1 gives the Gatherables Chest, Wardrobe, Kuku Cooler and Collectibles Chest\n"
                    "; 1,000 slots each. 0 leaves them at 10.\n"
                    "HousingChests1000=%d\n\n"
                    "; Total slots for Private Storage, purchased expansions included. 0 keeps the\n"
                    "; game's own size. The most is 1460.\n"
                    "PrivateStorageSlots=%d\n\n"
                    "; How many slots your expansions and story progress add to Private Storage.\n"
                    "; -1 has the mod read it from your save and use it from the next start, so\n"
                    "; the first start with a new PrivateStorageSlots can come out a little high.\n"
                    "PrivateStorageExpansions=%d\n",
                    v.leaveCapacityAlone ? 1 : 0, v.housingChests1000 ? 1 : 0, v.privateStorageSlots, v.privateStorageExpansions);
            fclose(f);
        }

        void Defaults()
        {
            g_values = Values{};
            for (int i = 0; i < kStorages; ++i)
            {
                ParseKey(kInfo[i].defKey, g_values.key[i]);
                ParsePad(kInfo[i].defPad, g_values.pad[i]);
            }
            ParseKey("Ctrl+F12", g_values.dumpKey);
        }

        void ReadIni(const std::wstring& path)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return;
            char line[512];
            while (fgets(line, sizeof line, f))
            {
                Trim(line);
                if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
                char* eq = strchr(line, '=');
                if (!eq) continue;
                *eq = 0;
                char* key = line;
                char* val = eq + 1;
                Trim(key);
                Trim(val);

                bool known = true;
                if (_stricmp(key, "Enabled") == 0) g_values.enabled = atoi(val) != 0;
                else if (_stricmp(key, "DebugLog") == 0) g_values.debugLog = atoi(val) != 0;
                else if (_stricmp(key, "LeaveCapacityAlone") == 0) g_values.leaveCapacityAlone = atoi(val) != 0;
                else if (_stricmp(key, "HousingChests1000") == 0) g_values.housingChests1000 = atoi(val) != 0;
                else if (_stricmp(key, "PrivateStorageSlots") == 0) g_values.privateStorageSlots = atoi(val);
                else if (_stricmp(key, "PrivateStorageExpansions") == 0) g_values.privateStorageExpansions = atoi(val);
                else if (_stricmp(key, "CapacityDumpKey") == 0)
                {
                    KeyBind k;
                    if (ParseKey(val, k)) g_values.dumpKey = k;
                    else LOG_ERR("[settings] CapacityDumpKey=%s is not a key this mod can bind; keeping the default", val);
                }
                else
                {
                    known = false;
                    for (int i = 0; i < kStorages && !known; ++i)
                    {
                        const size_t n = strlen(kInfo[i].key);
                        if (_strnicmp(key, kInfo[i].key, n) != 0) continue;
                        if (_stricmp(key + n, "Key") == 0)
                        {
                            known = true;
                            KeyBind k;
                            if (ParseKey(val, k)) g_values.key[i] = k;
                            else LOG_ERR("[settings] %s=%s is not a key this mod can bind; keeping the default", key, val);
                        }
                        else if (_stricmp(key + n, "Pad") == 0)
                        {
                            known = true;
                            PadBind p;
                            if (ParsePad(val, p)) g_values.pad[i] = p;
                            else LOG_ERR("[settings] %s=%s is not a controller combo; keeping the default", key, val);
                        }
                    }
                }
                if (!known) LOG_NOTE("[settings] %s is not a setting this version knows; ignored", key);
            }
            fclose(f);
        }

        // Two bindings on the same key or combo: the later one is turned off.
        void Deduplicate()
        {
            char t[64];
            for (int i = 0; i < kStorages; ++i)
                for (int j = 0; j < i; ++j)
                {
                    if (g_values.key[i].vk && g_values.key[i].vk == g_values.key[j].vk && g_values.key[i].mods == g_values.key[j].mods)
                    {
                        LOG_ERR("[settings] %sKey and %sKey are both %s; %sKey is turned off", kInfo[j].key, kInfo[i].key,
                                KeyText(g_values.key[i], t, sizeof t), kInfo[i].key);
                        g_values.key[i] = KeyBind{};
                    }
                    if (g_values.pad[i].press && g_values.pad[i].press == g_values.pad[j].press && g_values.pad[i].hold == g_values.pad[j].hold)
                    {
                        LOG_ERR("[settings] %sPad and %sPad are both %s; %sPad is turned off", kInfo[j].key, kInfo[i].key,
                                PadText(g_values.pad[i], t, sizeof t), kInfo[i].key);
                        g_values.pad[i] = PadBind{};
                    }
                }
            const KeyBind& d = g_values.dumpKey;
            for (int i = 0; i < kStorages; ++i)
                if (d.vk && d.vk == g_values.key[i].vk && d.mods == g_values.key[i].mods)
                {
                    LOG_ERR("[settings] CapacityDumpKey is the same as %sKey; the dump key is turned off", kInfo[i].key);
                    g_values.dumpKey = KeyBind{};
                    break;
                }
        }
    }

    const char* StorageKeyName(int storage) { return storage >= 0 && storage < kStorages ? kInfo[storage].key : "?"; }

    const char* KeyText(const KeyBind& k, char* out, size_t cap)
    {
        if (!k.vk) { snprintf(out, cap, "None"); return out; }
        char name[24] = {};
        const uint8_t vk = k.vk;
        if (vk >= VK_F1 && vk <= VK_F24) snprintf(name, sizeof name, "F%d", vk - VK_F1 + 1);
        else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) snprintf(name, sizeof name, "%c", vk);
        else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) snprintf(name, sizeof name, "Numpad%d", vk - VK_NUMPAD0);
        else
        {
            for (const NamedKey& n : kKeys)
                if (n.vk == vk) { snprintf(name, sizeof name, "%s", n.name); break; }
            if (!name[0]) snprintf(name, sizeof name, "0x%02X", vk);
        }
        snprintf(out, cap, "%s%s%s%s", (k.mods & kModCtrl) ? "Ctrl+" : "", (k.mods & kModShift) ? "Shift+" : "",
                 (k.mods & kModAlt) ? "Alt+" : "", name);
        return out;
    }

    const char* PadText(const PadBind& p, char* out, size_t cap)
    {
        if (!p.press) { snprintf(out, cap, "None"); return out; }
        size_t w = 0;
        out[0] = 0;
        static const char* const kOrder[] = {"LB", "RB", "LS", "RS", "A", "B", "X", "Y", "Up", "Down", "Left", "Right", "Start", "Back"};
        for (const char* n : kOrder)
        {
            const uint16_t bit = ButtonFromName(n);
            if (!(p.hold & bit)) continue;
            w += snprintf(out + w, cap - w, "%s+", n);
            if (w >= cap) return out;
        }
        for (const NamedButton& b : kButtons)
            if (b.bit == p.press) { snprintf(out + w, cap - w, "%s", b.name); break; }
        return out;
    }

    void Load()
    {
        Defaults();
        const std::wstring path = Paths::File(PSM_INI);
        const std::wstring old = Paths::File(L"PrivateStorageAnywhere.ini");
        const bool have = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (have)
            ReadIni(path);
        else if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES)
            ImportOld(old);
        Deduplicate();
        if (!have) WriteIni(path);

        Log::SetDebug(g_values.debugLog);
        if (g_values.imported)
            LOG_NOTE("[settings] no %s yet, so the bindings and capacity settings were taken from PrivateStorageAnywhere.ini and written to it",
                     Paths::FileUtf8(PSM_INI).c_str());
        else if (!have)
            LOG_NOTE("[settings] wrote a default %s", Paths::FileUtf8(PSM_INI).c_str());

        const Values& v = g_values;
        char a[64], b[64];
        for (int i = 0; i < kStorages; ++i)
            LOG_NOTE("[settings] %-15s key %-12s pad %s", kInfo[i].key, KeyText(v.key[i], a, sizeof a), PadText(v.pad[i], b, sizeof b));
        LOG_NOTE("[settings] Enabled=%d DebugLog=%d CapacityDumpKey=%s LeaveCapacityAlone=%d HousingChests1000=%d PrivateStorageSlots=%d "
                 "PrivateStorageExpansions=%d",
                 v.enabled ? 1 : 0, v.debugLog ? 1 : 0, KeyText(v.dumpKey, a, sizeof a), v.leaveCapacityAlone ? 1 : 0, v.housingChests1000 ? 1 : 0,
                 v.privateStorageSlots, v.privateStorageExpansions);
    }

    const Values& Get() { return g_values; }
}

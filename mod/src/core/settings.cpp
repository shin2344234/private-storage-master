#include "core/settings.h"

#include <Windows.h>
#include <atomic>
#include <string>
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
        struct StorageInfo
        {
            const char* key;        // ini prefix
            const char* label;      // in-game name
            const char* oldKey;     // PrivateStorageAnywhere.ini prefix, or nullptr
            const char* comment;    // what it is, for the written ini
            const char* defKey;
            const char* defPad;
        };
        const StorageInfo kInfo[kStorages] = {
            {"PrivateStorage", "Private Storage", "Private", "Private Storage, the camp storage box", "Ctrl+F1", "LB+LS"},
            {"Gatherables", "Gatherables Chest", "Gatherables", "Gatherables Chest (housing)", "Ctrl+F2", "LB+RS"},
            {"Dresser", "Wardrobe", "Dresser", "Wardrobe (housing)", "Ctrl+F3", ""},
            {"Refrigerator", "Kuku Cooler", "Refrigerator", "Kuku Cooler (housing)", "Ctrl+F4", ""},
            {"Collecting", "Collectibles Chest", "Collecting", "Collectibles Chest (housing)", "Ctrl+F5", ""},
            {"CampStraw", "Camp Straw", nullptr, "Camp Straw, the camp feed bin", "Ctrl+F6", ""},
            {"BirdFeed", "Bird Feed", nullptr, "Bird Feed, the camp bird feeder", "Ctrl+F7", ""},
            {"TownWarehouse", "Camp Provisions", nullptr, "Camp Provisions, the town warehouse for packaged trade goods", "Ctrl+F8", ""},
            {"AbyssGear", "Abyss Gear Storage", nullptr, "Abyss gear storage, the Kuku Pot bag", "Ctrl+F9", ""},
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

        void ImportOld(Values& out)
        {
            const std::string file = Paths::FileUtf8(L"PrivateStorageAnywhere.ini");
            char v[64];
            const auto get = [&](const char* key, const char* def) {
                GetPrivateProfileStringA("Settings", key, def, v, sizeof v, file.c_str());
                Trim(v);
                return v;
            };
            out.enabled = atoi(get("Enabled", "1")) != 0;
            out.slots[0] = atoi(get("PrivateStorageSlots", "0"));
            out.privateStorageExpansions = atoi(get("PrivateStorageExpansions", "-1"));

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
                out.key[i] = kb;

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
                out.pad[i] = pb;
            }
            char symbol[16];
            GetPrivateProfileStringA("Settings", "SymbolHotkey", "00", symbol, sizeof symbol, file.c_str());
            if (Hex8(symbol))
                LOG_NOTE("[settings] import: Symbol storage has no binding here. The game has no way to put anything in it.");
            out.imported = true;
        }

        // ---------------------------------------------------------------- writing
        bool WriteIni(const Values& v)
        {
            const std::wstring path = Paths::File(PSM_INI);
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f)
            {
                LOG_ERR("[settings] could not write %s", Paths::FileUtf8(PSM_INI).c_str());
                return false;
            }
            char a[64], b[64];
            fprintf(f, "[PrivateStorageMaster]\n\n");
            if (v.imported)
                fprintf(f, "; Written from your PrivateStorageAnywhere.ini the first time this mod ran.\n; That file is left as it was and is not read again.\n\n");
            fprintf(f,
                    "; Master Looter's Storage tab edits this file while the game runs.\n\n"
                    "; ------------------------------------------------------------------ general\n\n"
                    "; 1 turns the mod on. 0 loads it and does nothing. Takes effect next start.\n"
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
                fprintf(f, "; %s\n%sKey=%s\n%sPad=%s\n\n", kInfo[i].comment, kInfo[i].key, KeyText(v.key[i], a, sizeof a), kInfo[i].key,
                        PadText(v.pad[i], b, sizeof b));
            fprintf(f,
                    "; Writes the size and contents count of every storage to the log.\n"
                    "CapacityDumpKey=%s\n\n"
                    "; 1 keeps F1 to F12 from the game while a modifier your storage keys use is\n"
                    "; held, so a slip from one storage key to the next F key does not reach the\n"
                    "; game. Every other key goes through, so kicking, talking, trading and giving\n"
                    "; gifts with Ctrl held work as usual. Alt+F4 always closes the game.\n"
                    "HideKeysWithModifier=%d\n\n"
                    "; Turns HideKeysWithModifier on and off while you play, and saves it. None for no key.\n"
                    "HideKeysToggleKey=%s\n\n",
                    KeyText(v.dumpKey, a, sizeof a), v.hideKeysWithModifier ? 1 : 0, KeyText(v.hideKeysToggleKey, b, sizeof b));
            fprintf(f,
                    "; ------------------------------------------------------------------ sizes\n\n"
                    "; Size changes take effect the next time the game starts.\n\n"
                    "; 1 leaves every storage at the size the game (or another mod) gives it and\n"
                    "; ignores the sizes below. Use it with JSON capacity mods.\n"
                    "LeaveCapacityAlone=%d\n\n"
                    "; Slots for each storage, up to %d. 0 keeps the game's size, and a number\n"
                    "; below the game's size does the same; the mod never makes storage smaller.\n"
                    "; PrivateStorageSlots is the total, purchased expansions included.\n",
                    v.leaveCapacityAlone ? 1 : 0, kMaxSlots);
            for (int i = 0; i < kStorages; ++i)
            {
                if (kFixedSlots[i])
                    fprintf(f, "; %sSlots is always %d: the chest holds one of each collectible.\n", kInfo[i].key, kFixedSlots[i]);
                else
                    fprintf(f, "%sSlots=%d\n", kInfo[i].key, v.slots[i]);
            }
            fprintf(f,
                    "\n; How many slots your expansions and story progress add to Private Storage.\n"
                    "; -1 has the mod read it from your save and use it from the next start, so\n"
                    "; the first start with a new PrivateStorageSlots can come out a little high.\n"
                    "PrivateStorageExpansions=%d\n",
                    v.privateStorageExpansions);
            fprintf(f,
                    "\n[ItemStacks]\n\n"
                    "; How much of one item a single slot holds, as a multiple of the game's own\n"
                    "; limit. 1 leaves every stack alone. 5 turns a stack of 100 into 500 and a\n"
                    "; stack of 20 into 100, so the game's own differences between item kinds\n"
                    "; stay. Items the game does not stack, like gear and quest items, never\n"
                    "; start stacking, and no stack is taken past 999999. An item the game\n"
                    "; already lets you hold more of than that, such as money, is left alone.\n"
                    "; No game file is edited. A change made here takes effect the next time the\n"
                    "; game starts, because this file is read once. Raising it from Master Looter's\n"
                    "; Stacks tab takes hold straight away, as long as you are in free play with no\n"
                    "; storage open; a smaller number always waits for the next start.\n"
                    "; Your save does record the bigger stacks you build, so if you set\n"
                    "; this back to 1 a slot can still hold more than the game allows. It keeps\n"
                    "; what is in it until you take some out. Empty the big stacks first.\n"
                    "; Do not run a stack-size data mod such as Fat Stacks at the same time.\n"
                    "StackMultiplier=%d\n",
                    v.stackMultiplier);
            fprintf(f,
                    "\n[AutoStore]\n\n"
                    "; Loot straight into storage. Needs Master Looter 1.6.28 or later; on its own\n"
                    "; this section does nothing. Master Looter reports what it picks up (auto-loot,\n"
                    "; body searches, gathers), and each item goes to the first storage that is on\n"
                    "; here and takes it, in this order: Collectibles Chest, Abyss gear storage,\n"
                    "; Gatherables Chest, Kuku Cooler, Bird Feed, Camp Straw, Wardrobe, then Private\n"
                    "; Storage. The game's own rules decide what each one takes, and what none of\n"
                    "; them takes stays in your bag. What you pick up by hand, buy, craft or take out\n"
                    "; of storage is never moved. Master Looter's Storage tab has all of this under\n"
                    "; Store loot.\n"
                    "AutoStore=%d\n\n"
                    "; 1 lets that storage receive loot. Private Storage takes almost anything, so it\n"
                    "; is off by default; the Wardrobe is off so new gear stays with you to look at.\n"
                    "; Camp Provisions holds trade goods only and never receives loot.\n",
                    v.autoStore ? 1 : 0);
            for (int i = 0; i < kStorages; ++i)
                if (i != kTownWarehouse) fprintf(f, "AutoStore%s=%d\n", kInfo[i].key, v.autoStoreTo[i] ? 1 : 0);
            // Runs of three or more consecutive numbers are written as a range.
            char never[kNeverMoveMax * 7] = "";
            for (int i = 0; i < v.autoStoreNeverMoveCount;)
            {
                int end = i;
                while (end + 1 < v.autoStoreNeverMoveCount && v.autoStoreNeverMove[end + 1] == v.autoStoreNeverMove[end] + 1) ++end;
                char one[16];
                if (end - i >= 2) snprintf(one, sizeof one, "%s%u-%u", never[0] ? "," : "", v.autoStoreNeverMove[i], v.autoStoreNeverMove[end]);
                else { end = i; snprintf(one, sizeof one, "%s%u", never[0] ? "," : "", v.autoStoreNeverMove[i]); }
                strcat_s(never, one);
                i = end + 1;
            }
            fprintf(f,
                    "\n; 1 moves only the amount you just picked up, so food and potions you were\n"
                    "; already carrying stay in your bag. 0 moves the whole stack.\n"
                    "AutoStoreOnlyGained=%d\n\n"
                    "; Item numbers that never move, separated by commas, with ranges written as\n"
                    "; 1980-1999, at most 64. The default is every currency: your money (1980,\n"
                    "; copper and silver), the copper and silver pouches, gold bars, camp funds and\n"
                    "; supplies, Kuku currencies, faction contributions, refinement tokens, Marni\n"
                    "; tokens and the Hernand Bond. Master Looter's Never move list edits it by name.\n"
                    "AutoStoreNeverMove=%s\n",
                    v.autoStoreOnlyGained ? 1 : 0, never);
            fclose(f);
            return true;
        }

        // "1980-1999, 2003": item numbers and ranges, up to kNeverMoveMax numbers.
        // An empty value clears the list.
        void ParseItemList(const char* val, Values& out)
        {
            out.autoStoreNeverMoveCount = 0;
            const char* p = val;
            while (*p)
            {
                while (*p == ' ' || *p == ',' || *p == '\t') ++p;
                if (!*p) break;
                char* end = nullptr;
                const unsigned long n = strtoul(p, &end, 10);
                if (end == p) { LOG_ERR("[settings] AutoStoreNeverMove=%s has something that is not an item number; the rest is ignored", val); break; }
                unsigned long last = n;
                p = end;
                if (*p == '-')
                {
                    last = strtoul(p + 1, &end, 10);
                    if (end == p + 1 || last < n) { LOG_ERR("[settings] AutoStoreNeverMove=%s has a range that does not read; the rest is ignored", val); break; }
                    p = end;
                }
                for (unsigned long k = n; k <= last && k < 0xFFFF; ++k)
                {
                    if (out.autoStoreNeverMoveCount == kNeverMoveMax)
                    {
                        LOG_ERR("[settings] AutoStoreNeverMove holds at most %d items; %lu and everything after it are left off, "
                                "and the ini will be saved without them",
                                kNeverMoveMax, k);
                        return;
                    }
                    out.autoStoreNeverMove[out.autoStoreNeverMoveCount++] = static_cast<uint16_t>(k);
                }
            }
        }

        // AutoStore<Storage>=0|1. False when the rest of the key names no storage.
        bool AutoStoreSwitch(const char* rest, const char* val, Values& out)
        {
            for (int i = 0; i < kStorages; ++i)
                if (_stricmp(rest, kInfo[i].key) == 0) { out.autoStoreTo[i] = atoi(val) != 0; return true; }
            return false;
        }

        void ReadIni(Values& out)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, Paths::File(PSM_INI).c_str(), L"r") != 0 || !f) return;
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
                if (_stricmp(key, "Enabled") == 0) out.enabled = atoi(val) != 0;
                else if (_stricmp(key, "DebugLog") == 0) out.debugLog = atoi(val) != 0;
                else if (_stricmp(key, "LeaveCapacityAlone") == 0) out.leaveCapacityAlone = atoi(val) != 0;
                else if (_stricmp(key, "HideKeysWithModifier") == 0) out.hideKeysWithModifier = atoi(val) != 0;
                // Test build 1 wrote these three before sizes went per storage.
                else if (_stricmp(key, "HousingChests1000") == 0) { for (int i = 1; i <= 4; ++i) out.slots[i] = atoi(val) ? 1000 : 0; }
                else if (_stricmp(key, "CampStorage1000") == 0) { for (int i = 5; i <= 8; ++i) out.slots[i] = atoi(val) ? 1000 : 0; }
                else if (_stricmp(key, "PrivateStorageExpansions") == 0) out.privateStorageExpansions = atoi(val);
                else if (_stricmp(key, "StackMultiplier") == 0) out.stackMultiplier = atoi(val);
                else if (_stricmp(key, "AutoStore") == 0) out.autoStore = atoi(val) != 0;
                else if (_stricmp(key, "AutoStoreOnlyGained") == 0) out.autoStoreOnlyGained = atoi(val) != 0;
                else if (_stricmp(key, "AutoStoreNeverMove") == 0) ParseItemList(val, out);
                else if (_strnicmp(key, "AutoStore", 9) == 0 && AutoStoreSwitch(key + 9, val, out)) {}
                else if (_stricmp(key, "CapacityDumpKey") == 0)
                {
                    KeyBind k;
                    if (ParseKey(val, k)) out.dumpKey = k;
                    else LOG_ERR("[settings] CapacityDumpKey=%s is not a key this mod can bind; keeping the default", val);
                }
                else if (_stricmp(key, "HideKeysToggleKey") == 0)
                {
                    KeyBind k;
                    if (ParseKey(val, k)) out.hideKeysToggleKey = k;
                    else LOG_ERR("[settings] HideKeysToggleKey=%s is not a key this mod can bind; keeping the default", val);
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
                            if (ParseKey(val, k)) out.key[i] = k;
                            else LOG_ERR("[settings] %s=%s is not a key this mod can bind; keeping the default", key, val);
                        }
                        else if (_stricmp(key + n, "Pad") == 0)
                        {
                            known = true;
                            PadBind p;
                            if (ParsePad(val, p)) out.pad[i] = p;
                            else LOG_ERR("[settings] %s=%s is not a controller combo; keeping the default", key, val);
                        }
                        else if (_stricmp(key + n, "Slots") == 0)
                        {
                            known = true;
                            out.slots[i] = atoi(val);
                        }
                    }
                }
                if (!known) LOG_NOTE("[settings] %s is not a setting this version knows; ignored", key);
            }
            fclose(f);
        }

        void Clamp(Values& v)
        {
            for (int i = 0; i < kStorages; ++i)
            {
                int& s = v.slots[i];
                if (s < 0) s = 0;
                if (s > kMaxSlots) s = kMaxSlots;
                if (kFixedSlots[i]) s = kFixedSlots[i];
            }
            if (v.stackMultiplier < 1) v.stackMultiplier = 1;
            if (v.stackMultiplier > kMaxStackMultiplier) v.stackMultiplier = kMaxStackMultiplier;
            if (v.privateStorageExpansions < -1) v.privateStorageExpansions = -1;
            if (v.privateStorageExpansions > kMaxSlots) v.privateStorageExpansions = kMaxSlots;
            v.autoStoreTo[kTownWarehouse] = false;
            if (v.autoStoreNeverMoveCount < 0) v.autoStoreNeverMoveCount = 0;
            if (v.autoStoreNeverMoveCount > kNeverMoveMax) v.autoStoreNeverMoveCount = kNeverMoveMax;
        }

        // Two bindings on the same key or combo: the later one is turned off.
        void Deduplicate(Values& v)
        {
            char t[64];
            for (int i = 0; i < kStorages; ++i)
                for (int j = 0; j < i; ++j)
                {
                    if (v.key[i].vk && v.key[i].vk == v.key[j].vk && v.key[i].mods == v.key[j].mods)
                    {
                        LOG_ERR("[settings] %sKey and %sKey are both %s; %sKey is turned off", kInfo[j].key, kInfo[i].key,
                                KeyText(v.key[i], t, sizeof t), kInfo[i].key);
                        v.key[i] = KeyBind{};
                    }
                    if (v.pad[i].press && v.pad[i].press == v.pad[j].press && v.pad[i].hold == v.pad[j].hold)
                    {
                        LOG_ERR("[settings] %sPad and %sPad are both %s; %sPad is turned off", kInfo[j].key, kInfo[i].key,
                                PadText(v.pad[i], t, sizeof t), kInfo[i].key);
                        v.pad[i] = PadBind{};
                    }
                }
            for (int i = 0; i < kStorages; ++i)
                if (v.dumpKey.vk && v.dumpKey.vk == v.key[i].vk && v.dumpKey.mods == v.key[i].mods)
                {
                    LOG_ERR("[settings] CapacityDumpKey is the same as %sKey; the dump key is turned off", kInfo[i].key);
                    v.dumpKey = KeyBind{};
                    break;
                }
            const KeyBind& h = v.hideKeysToggleKey;
            bool clash = h.vk && h.vk == v.dumpKey.vk && h.mods == v.dumpKey.mods;
            for (int i = 0; i < kStorages && !clash; ++i) clash = h.vk && h.vk == v.key[i].vk && h.mods == v.key[i].mods;
            if (clash)
            {
                LOG_ERR("[settings] HideKeysToggleKey=%s is already bound; the toggle key is turned off", KeyText(h, t, sizeof t));
                v.hideKeysToggleKey = KeyBind{};
            }
        }

        // Published copies are never freed: a reader may still hold the old one,
        // and a copy is a few hundred bytes changed a handful of times a session.
        std::atomic<const Values*> g_cur{nullptr};
        const Values* g_startup = nullptr;
        SRWLOCK g_writeLock = SRWLOCK_INIT;

        void Publish(const Values& v)
        {
            g_cur.store(new Values(v));
            Log::SetDebug(v.debugLog);
        }

        void LogValues(const Values& v)
        {
            char a[64], b[64];
            // The line per storage is DebugLog only; the summary below always goes out.
            for (int i = 0; i < kStorages; ++i)
                LOG("[settings] %-15s key %-12s pad %-10s slots %d", kInfo[i].key, KeyText(v.key[i], a, sizeof a), PadText(v.pad[i], b, sizeof b),
                    v.slots[i]);
            LOG_NOTE("[settings] Enabled=%d DebugLog=%d CapacityDumpKey=%s HideKeysWithModifier=%d HideKeysToggleKey=%s LeaveCapacityAlone=%d "
                     "PrivateStorageExpansions=%d", v.enabled ? 1 : 0, v.debugLog ? 1 : 0, KeyText(v.dumpKey, a, sizeof a),
                     v.hideKeysWithModifier ? 1 : 0, KeyText(v.hideKeysToggleKey, b, sizeof b), v.leaveCapacityAlone ? 1 : 0,
                     v.privateStorageExpansions);
            if (v.stackMultiplier > 1) LOG_NOTE("[settings] StackMultiplier=%d", v.stackMultiplier);
            if (v.autoStore)
            {
                char to[128] = "";
                for (int i = 0; i < kStorages; ++i)
                    if (v.autoStoreTo[i]) { if (to[0]) strcat_s(to, ","); strcat_s(to, kInfo[i].key); }
                LOG_NOTE("[settings] AutoStore=1 to %s, OnlyGained=%d, %d never-move items", to[0] ? to : "nothing",
                         v.autoStoreOnlyGained ? 1 : 0, v.autoStoreNeverMoveCount);
            }
        }
    }

    const char* StorageKeyName(int storage) { return storage >= 0 && storage < kStorages ? kInfo[storage].key : "?"; }
    const char* StorageLabel(int storage) { return storage >= 0 && storage < kStorages ? kInfo[storage].label : "?"; }

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

    Values Defaults()
    {
        Values v;
        for (int i = 0; i < kStorages; ++i)
        {
            ParseKey(kInfo[i].defKey, v.key[i]);
            ParsePad(kInfo[i].defPad, v.pad[i]);
        }
        ParseKey("Ctrl+F12", v.dumpKey);
        ParseKey("Ctrl+F10", v.hideKeysToggleKey);   // F10 is in the game's debug group only
        return v;
    }

    void Load()
    {
        Values v = Defaults();
        const bool have = GetFileAttributesW(Paths::File(PSM_INI).c_str()) != INVALID_FILE_ATTRIBUTES;
        if (have)
            ReadIni(v);
        else if (GetFileAttributesW(Paths::File(L"PrivateStorageAnywhere.ini").c_str()) != INVALID_FILE_ATTRIBUTES)
            ImportOld(v);
        Clamp(v);
        Deduplicate(v);
        if (!have) WriteIni(v);
        g_startup = new Values(v);
        Publish(v);

        if (v.imported)
            LOG_NOTE("[settings] no %s yet, so the bindings and capacity settings were taken from PrivateStorageAnywhere.ini and written to it",
                     Paths::FileUtf8(PSM_INI).c_str());
        else if (!have)
            LOG_NOTE("[settings] wrote a default %s", Paths::FileUtf8(PSM_INI).c_str());
        LogValues(v);
    }

    const Values& Get()
    {
        static const Values kEmpty{};
        const Values* v = g_cur.load();
        return v ? *v : kEmpty;
    }

    const Values& Startup() { return g_startup ? *g_startup : Get(); }

    namespace
    {
        // Checks v, then publishes it and writes the ini. The caller holds g_writeLock.
        // 0 refused, 1 applied and saved, 2 applied but not saved.
        int CommitLocked(Values v, char* why, size_t whyLen)
        {
            for (int i = 0; i < kStorages; ++i)
            {
                if (v.key[i].vk && !Bindable(v.key[i].vk))
                {
                    if (why) snprintf(why, whyLen, "%s: mouse buttons and modifier keys cannot be bound", kInfo[i].label);
                    return 0;
                }
                if (v.pad[i].press && (v.pad[i].hold & v.pad[i].press))
                {
                    if (why) snprintf(why, whyLen, "%s: the pressed button is also a held one", kInfo[i].label);
                    return 0;
                }
            }
            if (v.dumpKey.vk && !Bindable(v.dumpKey.vk))
            {
                if (why) snprintf(why, whyLen, "Capacity dump: mouse buttons and modifier keys cannot be bound");
                return 0;
            }
            if (v.hideKeysToggleKey.vk && !Bindable(v.hideKeysToggleKey.vk))
            {
                if (why) snprintf(why, whyLen, "Held-key toggle: mouse buttons and modifier keys cannot be bound");
                return 0;
            }
            Clamp(v);
            Deduplicate(v);
            v.imported = Get().imported;
            Publish(v);
            return WriteIni(v) ? 1 : 2;
        }

        bool Report(int result, char* why, size_t whyLen)
        {
            if (result == 0) return false;
            LOG("[settings] changed in game%s", result == 1 ? " and saved" : ", but the ini could not be written");
            if (result == 2 && why) snprintf(why, whyLen, "applied, but %s could not be written", Paths::FileUtf8(PSM_INI).c_str());
            return result == 1;
        }
    }

    bool Apply(const Values& in, char* why, size_t whyLen)
    {
        if (why && whyLen) why[0] = 0;
        AcquireSRWLockExclusive(&g_writeLock);
        const int result = CommitLocked(in, why, whyLen);
        ReleaseSRWLockExclusive(&g_writeLock);
        return Report(result, why, whyLen);
    }

    bool Update(const std::function<void(Values&)>& change, char* why, size_t whyLen)
    {
        if (why && whyLen) why[0] = 0;
        AcquireSRWLockExclusive(&g_writeLock);
        Values v = Get();
        change(v);
        const int result = CommitLocked(v, why, whyLen);
        ReleaseSRWLockExclusive(&g_writeLock);
        return Report(result, why, whyLen);
    }

    void Reload()
    {
        Values v = Defaults();
        ReadIni(v);
        Clamp(v);
        Deduplicate(v);
        AcquireSRWLockExclusive(&g_writeLock);
        Publish(v);
        ReleaseSRWLockExclusive(&g_writeLock);
        LOG_NOTE("[settings] read %s again", Paths::FileUtf8(PSM_INI).c_str());
        LogValues(v);
    }

    bool RestartNeeded()
    {
        const Values& a = Get();
        const Values& b = Startup();
        // stackMultiplier is left out on purpose: a raise can take effect without a
        // restart, so whether stacks need one is storage/stacks' answer to give,
        // through StackGetStatus, not a comparison against the launch.
        if (a.enabled != b.enabled || a.leaveCapacityAlone != b.leaveCapacityAlone || a.privateStorageExpansions != b.privateStorageExpansions)
            return true;
        for (int i = 0; i < kStorages; ++i)
            if (a.slots[i] != b.slots[i]) return true;
        return false;
    }
}

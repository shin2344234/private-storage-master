#pragma once
#include <cstddef>
#include <cstdint>

// PrivateStorageMaster.ini.
//
// Keys are written as names: "F4", "Ctrl+F1", "Shift+I". Controller combos are
// XInput button names joined with "+": every button but the last is held, the
// last one is pressed ("LB+LS" is hold LB, press the left stick).
//
// With no PrivateStorageMaster.ini beside the plugin, a PrivateStorageAnywhere.ini
// there is read for its bindings and capacity settings, and the new file is
// written with them.
//
// Settings can change while the game runs (Master Looter's Storage tab). Get()
// hands out the current copy; a change publishes a new copy and the old one is
// kept alive, so a reader on another thread never sees a half-written value.
namespace psm::Settings
{
    enum : uint8_t { kModCtrl = 1, kModShift = 2, kModAlt = 4 };

    struct KeyBind
    {
        uint8_t vk = 0;     // 0: no key
        uint8_t mods = 0;   // kMod* bits that must be held, and no others
    };

    struct PadBind
    {
        uint16_t hold = 0;     // XINPUT_GAMEPAD_* bits held first
        uint16_t press = 0;    // the one button that fires; 0: no combo
    };

    // Order matches storage::kChests and the capacity targets.
    inline constexpr int kStorages = 9;
    inline constexpr int kMaxSlots = 1460;   // the slot array every storage has on 2.02
    // Sizes that are not a setting. The Collectibles Chest holds one of each of the
    // 958 collectibles in its data, so it is always given exactly that.
    inline constexpr int kFixedSlots[kStorages] = {0, 0, 0, 0, 958, 0, 0, 0, 0};

    struct Values
    {
        bool enabled = true;
        bool debugLog = false;
        KeyBind key[kStorages];
        PadBind pad[kStorages];
        KeyBind dumpKey;
        // While Ctrl (or whatever modifier a storage key uses) is held, keep every
        // other key from the game too, so a slip off Ctrl+F1 cannot fire a skill.
        bool hideKeysWithModifier = true;
        // Turns hideKeysWithModifier on and off in game and saves it. Ctrl is the
        // game's guard and lock-on key, so a player may want keys back mid-fight.
        // Not in PsmSettings: the API struct shipped with 1.0.0 and keeps its size,
        // and FromC starts from the current values, so Master Looter keeps it too.
        KeyBind hideKeysToggleKey;

        bool leaveCapacityAlone = false;
        // Slots per storage; 0 keeps the game's size. Storage 0 (Private Storage)
        // counts purchased expansions in its total, the others are the base size.
        int  slots[kStorages] = {0, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
        int  privateStorageExpansions = -1;  // -1: learn them from the save
        bool imported = false;               // settings came from PrivateStorageAnywhere.ini
    };

    void Load();                  // once, at startup
    const Values& Get();          // current settings
    const Values& Startup();      // what this launch started with (Enabled and sizes use these)
    Values Defaults();

    // Checks, publishes and writes the ini. False with a reason when a value is refused.
    bool Apply(const Values& v, char* why, size_t whyLen);
    // Reads the ini again and publishes it.
    void Reload();
    // True when Enabled or a size setting differs from what this launch started with.
    bool RestartNeeded();

    // "Ctrl+F1", "LB+LS", "None".
    const char* KeyText(const KeyBind& k, char* out, size_t cap);
    const char* PadText(const PadBind& p, char* out, size_t cap);

    // The ini name of each storage, "PrivateStorage", "Gatherables", ...
    const char* StorageKeyName(int storage);
    const char* StorageLabel(int storage);   // the in-game name
}

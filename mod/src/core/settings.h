#pragma once
#include <cstddef>
#include <cstdint>

// PrivateStorageMaster.ini, read once at startup.
//
// Keys are written as names: "F4", "Ctrl+F1", "Shift+I". Controller combos are
// XInput button names joined with "+": every button but the last is held, the
// last one is pressed ("LB+LS" is hold LB, press the left stick).
//
// With no PrivateStorageMaster.ini beside the plugin, a PrivateStorageAnywhere.ini
// there is read for its bindings and capacity settings, and the new file is
// written with them.
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

    // Order matches storage::kChests.
    inline constexpr int kStorages = 9;

    struct Values
    {
        bool enabled = true;
        bool debugLog = false;
        KeyBind key[kStorages];
        PadBind pad[kStorages];
        KeyBind dumpKey;

        bool leaveCapacityAlone = false;
        bool housingChests1000 = true;
        int  privateStorageSlots = 0;        // 0: the game's own capacity
        int  privateStorageExpansions = -1;  // -1: learn them from the save
        bool imported = false;               // settings came from PrivateStorageAnywhere.ini
    };

    void Load();
    const Values& Get();

    // "Ctrl+F1", "LB + LS", "none".
    const char* KeyText(const KeyBind& k, char* out, size_t cap);
    const char* PadText(const PadBind& p, char* out, size_t cap);

    // The ini name of each storage, "PrivateStorage", "Gatherables", ...
    const char* StorageKeyName(int storage);
}

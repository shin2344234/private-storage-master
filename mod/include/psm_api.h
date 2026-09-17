/* Private Storage Master, in-process interface.
 *
 * Another plugin in the game process (Master Looter's Storage tab) finds
 * PrivateStorageMaster.asi with GetModuleHandleW and each function below with
 * GetProcAddress. Check PsmApiVersion() first and use nothing if it differs
 * from PSM_API_VERSION. Every struct starts with its own size; set it before
 * passing one in, and a call with a size it does not know returns 0.
 *
 * Plain C, no allocation across the boundary, strings copied into the
 * caller's buffers. Everything is safe to call from any thread, every frame.
 */
#pragma once
#include <stdint.h>

#ifndef PSM_API
#define PSM_API   /* the plugin defines it as dllexport while building */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PSM_API_VERSION 1
#define PSM_STORAGES    9       /* see PsmStorageName for the order */
#define PSM_MAX_SLOTS   1460

#define PSM_MOD_CTRL  1
#define PSM_MOD_SHIFT 2
#define PSM_MOD_ALT   4

typedef struct PsmKey
{
    uint8_t vk;     /* Windows virtual-key code, 0 for none */
    uint8_t mods;   /* PSM_MOD_* that must be held, and no others */
} PsmKey;

typedef struct PsmPad
{
    uint16_t hold;  /* XINPUT_GAMEPAD_* held first */
    uint16_t press; /* the single button that fires, 0 for none */
} PsmPad;

typedef struct PsmSettings
{
    uint32_t size;                          /* sizeof(PsmSettings) */
    int32_t  enabled;                       /* next launch */
    int32_t  debugLog;
    PsmKey   keys[PSM_STORAGES];
    PsmPad   pads[PSM_STORAGES];
    PsmKey   dumpKey;
    int32_t  leaveCapacityAlone;            /* next launch */
    int32_t  slots[PSM_STORAGES];           /* next launch; 0 keeps the game's size */
    int32_t  privateStorageExpansions;      /* next launch; -1 learns them from the save */
} PsmSettings;

typedef struct PsmStatus
{
    uint32_t size;                          /* sizeof(PsmStatus) */
    int32_t  enabled;                       /* this launch */
    int32_t  storageReady;                  /* keys and combos are live */
    int32_t  capacityHooked;                /* sizes were applied this launch */
    int32_t  keyWindowFound;
    int32_t  padSlot;                       /* -1 with no controller */
    int32_t  openStorage;                   /* -1 when none */
    int32_t  oldModLoaded;                  /* PrivateStorageAnywhere.asi is in the process */
    int32_t  imported;                      /* settings came from PrivateStorageAnywhere.ini */
    int32_t  restartNeeded;                 /* a next-launch setting differs from this launch */
    int32_t  learnedExpansions;             /* Private Storage slots from expansions and story */
    char     version[32];
    char     gameVersion[16];
    char     lastError[256];
} PsmStatus;

typedef struct PsmSize
{
    uint32_t size;                          /* sizeof(PsmSize) */
    int32_t  known;                         /* the game has read this storage's record */
    int32_t  gameDefault;
    int32_t  gameMax;
    int32_t  appliedDefault;                /* this launch */
    int32_t  liveCapacity;                  /* -1 with no save loaded */
    int32_t  liveUsed;                      /* filled slots, -1 with no save loaded */
    int32_t  slotArray;
    int32_t  extras;                        /* expansions bought plus story slots */
} PsmSize;

PSM_API int         PsmApiVersion(void);
PSM_API const char* PsmStorageName(int storage);    /* in-game name, "Private Storage" */
PSM_API const char* PsmStorageKey(int storage);     /* ini prefix, "PrivateStorage" */
PSM_API int         PsmGetStatus(PsmStatus* out);
PSM_API int         PsmGetSettings(PsmSettings* out);
PSM_API int         PsmGetDefaults(PsmSettings* out);
/* Applies what can change live, saves the ini. 0 with a reason in why. */
PSM_API int         PsmApplySettings(const PsmSettings* in, char* why, int whyLen);
PSM_API int         PsmReloadSettings(void);
PSM_API int         PsmGetSize(int storage, PsmSize* out);
PSM_API void        PsmWriteDump(void);
/* Ignore storage keys and combos for this long; call it every frame a menu has input. */
PSM_API void        PsmPauseInput(uint32_t ms);
/* A size that is not a setting (the Collectibles Chest), or 0. Added after
 * interface 1 shipped to Master Looter, so look it up as optional. */
PSM_API int         PsmFixedSlots(int storage);
/* "Ctrl+F1", "LB+LS", "None". */
PSM_API int         PsmKeyText(PsmKey key, char* out, int outLen);
PSM_API int         PsmPadText(PsmPad pad, char* out, int outLen);

#ifdef __cplusplus
}
#endif

// The exported interface in include/psm_api.h. Thin: it copies between the C
// structs and Settings, and asks the modules for status.
#define PSM_API __declspec(dllexport)
#include "psm_api.h"

#include <Windows.h>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/settings.h"
#include "storage/capacity.h"
#include "storage/deposit.h"
#include "storage/pad.h"
#include "storage/storage.h"
#include "version.h"

#define PSM_EXPORT extern "C" PSM_API

// snprintf truncates without complaining, so a game string that outgrows the
// field just quietly loses its tail. PsmStatus is part of the shipped API and
// cannot be widened, so fail the build instead.
static_assert(sizeof PSM_GAME <= sizeof(PsmStatus::gameVersion),
              "PSM_GAME does not fit PsmStatus::gameVersion");
static_assert(sizeof PSM_VERSION <= sizeof(PsmStatus::version),
              "PSM_VERSION does not fit PsmStatus::version");

namespace
{
    using psm::Settings::Values;

    void ToC(const Values& v, PsmSettings* out)
    {
        memset(out, 0, sizeof *out);
        out->size = sizeof *out;
        out->enabled = v.enabled;
        out->debugLog = v.debugLog;
        for (int i = 0; i < PSM_STORAGES; ++i)
        {
            out->keys[i] = {v.key[i].vk, v.key[i].mods};
            out->pads[i] = {v.pad[i].hold, v.pad[i].press};
            out->slots[i] = v.slots[i];
        }
        out->dumpKey = {v.dumpKey.vk, v.dumpKey.mods};
        out->leaveCapacityAlone = v.leaveCapacityAlone;
        out->privateStorageExpansions = v.privateStorageExpansions;
    }

    Values FromC(const PsmSettings* in)
    {
        Values v = psm::Settings::Get();
        v.enabled = in->enabled != 0;
        v.debugLog = in->debugLog != 0;
        for (int i = 0; i < PSM_STORAGES; ++i)
        {
            v.key[i] = {in->keys[i].vk, static_cast<uint8_t>(in->keys[i].mods & 7)};
            v.pad[i] = {in->pads[i].hold, in->pads[i].press};
            v.slots[i] = in->slots[i];
        }
        v.dumpKey = {in->dumpKey.vk, static_cast<uint8_t>(in->dumpKey.mods & 7)};
        v.leaveCapacityAlone = in->leaveCapacityAlone != 0;
        v.privateStorageExpansions = in->privateStorageExpansions;
        return v;
    }
}

PSM_EXPORT int PsmApiVersion(void) { return PSM_API_VERSION; }

PSM_EXPORT const char* PsmStorageName(int storage) { return psm::Settings::StorageLabel(storage); }
PSM_EXPORT const char* PsmStorageKey(int storage) { return psm::Settings::StorageKeyName(storage); }

PSM_EXPORT int PsmGetStatus(PsmStatus* out)
{
    if (!out || out->size != sizeof *out) return 0;
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->enabled = psm::Settings::Startup().enabled;
    out->storageReady = psm::storage::Ready();
    out->capacityHooked = psm::capacity::Hooked();
    out->keyWindowFound = psm::storage::KeyWindowFound();
    out->padSlot = psm::pad::Slot();
    out->openStorage = psm::storage::OpenStorage();
    out->oldModLoaded = GetModuleHandleW(L"PrivateStorageAnywhere.asi") != nullptr;
    out->imported = psm::Settings::Get().imported;
    out->restartNeeded = psm::Settings::RestartNeeded();
    out->learnedExpansions = psm::capacity::LearnedPrivateExtras();
    snprintf(out->version, sizeof out->version, "%s", PSM_VERSION);
    snprintf(out->gameVersion, sizeof out->gameVersion, "%s", PSM_GAME);
    psm::Log::LastError(out->lastError, sizeof out->lastError);
    return 1;
}

PSM_EXPORT int PsmGetSettings(PsmSettings* out)
{
    if (!out || out->size != sizeof *out) return 0;
    ToC(psm::Settings::Get(), out);
    return 1;
}

PSM_EXPORT int PsmGetDefaults(PsmSettings* out)
{
    if (!out || out->size != sizeof *out) return 0;
    ToC(psm::Settings::Defaults(), out);
    return 1;
}

PSM_EXPORT int PsmApplySettings(const PsmSettings* in, char* why, int whyLen)
{
    if (why && whyLen > 0) why[0] = 0;
    if (!in || in->size != sizeof *in)
    {
        if (why && whyLen > 0) snprintf(why, whyLen, "settings struct size %u, expected %zu", in ? in->size : 0, sizeof *in);
        return 0;
    }
    return psm::Settings::Apply(FromC(in), why, why && whyLen > 0 ? static_cast<size_t>(whyLen) : 0) ? 1 : 0;
}

PSM_EXPORT int PsmReloadSettings(void)
{
    psm::Settings::Reload();
    return 1;
}

PSM_EXPORT int PsmGetSize(int storage, PsmSize* out)
{
    if (!out || out->size != sizeof *out || storage < 0 || storage >= PSM_STORAGES) return 0;
    psm::capacity::SizeInfo s;
    psm::capacity::GetSize(storage, s);
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->known = s.known;
    out->gameDefault = s.gameDefault;
    out->gameMax = s.gameMax;
    out->appliedDefault = s.appliedDefault;
    out->liveCapacity = s.liveCapacity;
    out->liveUsed = s.liveUsed;
    out->slotArray = s.slotArray;
    out->extras = s.extras;
    return 1;
}

PSM_EXPORT void PsmWriteDump(void) { psm::capacity::RequestDump(); }

PSM_EXPORT int PsmFixedSlots(int storage)
{
    return storage >= 0 && storage < PSM_STORAGES ? psm::Settings::kFixedSlots[storage] : 0;
}

PSM_EXPORT void PsmPauseInput(uint32_t ms) { psm::storage::PauseInput(ms > 2000 ? 2000 : ms); }

PSM_EXPORT int PsmHidingKeys(void) { return psm::storage::HidingKeys() ? 1 : 0; }

PSM_EXPORT int PsmGetKeyBlock(PsmKeyBlock* out, int defaults)
{
    if (!out || out->size != sizeof *out) return 0;
    const Values v = defaults ? psm::Settings::Defaults() : psm::Settings::Get();
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->on = v.hideKeysWithModifier;
    out->toggleKey = {v.hideKeysToggleKey.vk, v.hideKeysToggleKey.mods};
    return 1;
}

PSM_EXPORT int PsmApplyKeyBlock(const PsmKeyBlock* in, char* why, int whyLen)
{
    if (why && whyLen > 0) why[0] = 0;
    if (!in || in->size != sizeof *in)
    {
        if (why && whyLen > 0) snprintf(why, whyLen, "key block struct size %u, expected %zu", in ? in->size : 0, sizeof *in);
        return 0;
    }
    Values v = psm::Settings::Get();
    v.hideKeysWithModifier = in->on != 0;
    v.hideKeysToggleKey = {in->toggleKey.vk, static_cast<uint8_t>(in->toggleKey.mods & 7)};
    return psm::Settings::Apply(v, why, why && whyLen > 0 ? static_cast<size_t>(whyLen) : 0) ? 1 : 0;
}

static_assert(PSM_DEPOSIT_STORED == psm::deposit::kStored && PSM_DEPOSIT_BUSY == psm::deposit::kBusy,
              "PSM_DEPOSIT_* and deposit::Reason must match");
static_assert(sizeof(PsmDepositResult) == sizeof(psm::deposit::Result), "PsmDepositResult and deposit::Result must match");

PSM_EXPORT int PsmGetAutoStore(PsmAutoStore* out, int defaults)
{
    if (!out || out->size != sizeof *out) return 0;
    const Values v = defaults ? psm::Settings::Defaults() : psm::Settings::Get();
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->enabled = v.autoStore;
    for (int i = 0; i < PSM_STORAGES; ++i) out->storages[i] = v.autoStoreTo[i];
    out->onlyGained = v.autoStoreOnlyGained;
    out->available = psm::deposit::Available();
    return 1;
}

PSM_EXPORT int PsmApplyAutoStore(const PsmAutoStore* in, char* why, int whyLen)
{
    if (why && whyLen > 0) why[0] = 0;
    if (!in || in->size != sizeof *in)
    {
        if (why && whyLen > 0) snprintf(why, whyLen, "auto-store struct size %u, expected %zu", in ? in->size : 0, sizeof *in);
        return 0;
    }
    Values v = psm::Settings::Get();
    v.autoStore = in->enabled != 0;
    for (int i = 0; i < PSM_STORAGES; ++i) v.autoStoreTo[i] = in->storages[i] != 0;
    v.autoStoreOnlyGained = in->onlyGained != 0;
    return psm::Settings::Apply(v, why, why && whyLen > 0 ? static_cast<size_t>(whyLen) : 0) ? 1 : 0;
}

static_assert(PSM_NEVER_MOVE_MAX == psm::Settings::kNeverMoveMax, "PSM_NEVER_MOVE_MAX and Settings::kNeverMoveMax must match");

PSM_EXPORT int PsmGetNeverMove(uint16_t* items, int max, int defaults)
{
    if (!items || max <= 0) return 0;
    const Values v = defaults ? psm::Settings::Defaults() : psm::Settings::Get();
    const int n = v.autoStoreNeverMoveCount < max ? v.autoStoreNeverMoveCount : max;
    memcpy(items, v.autoStoreNeverMove, sizeof(uint16_t) * n);
    return n;
}

PSM_EXPORT int PsmApplyNeverMove(const uint16_t* items, int count, char* why, int whyLen)
{
    if (why && whyLen > 0) why[0] = 0;
    if (count < 0 || count > PSM_NEVER_MOVE_MAX || (count > 0 && !items))
    {
        if (why && whyLen > 0) snprintf(why, whyLen, "%d items; the list holds at most %d", count, PSM_NEVER_MOVE_MAX);
        return 0;
    }
    uint16_t sorted[PSM_NEVER_MOVE_MAX];
    int n = 0;
    for (int i = 0; i < count; ++i)
    {
        if (items[i] == 0xFFFF) continue;
        // Insertion into a sorted list, dropping repeats, so the ini can write runs as ranges.
        int at = 0;
        while (at < n && sorted[at] < items[i]) ++at;
        if (at < n && sorted[at] == items[i]) continue;
        memmove(sorted + at + 1, sorted + at, sizeof(uint16_t) * (n - at));
        sorted[at] = items[i];
        ++n;
    }
    Values v = psm::Settings::Get();
    memcpy(v.autoStoreNeverMove, sorted, sizeof(uint16_t) * n);
    v.autoStoreNeverMoveCount = n;
    return psm::Settings::Apply(v, why, why && whyLen > 0 ? static_cast<size_t>(whyLen) : 0) ? 1 : 0;
}

PSM_EXPORT int PsmDeposit(uint16_t item, int64_t gained) { return psm::deposit::Queue(item, gained) ? 1 : 0; }

PSM_EXPORT int PsmDepositResults(PsmDepositResult* out, int max)
{
    if (!out || max <= 0) return 0;
    return psm::deposit::TakeResults(reinterpret_cast<psm::deposit::Result*>(out), max);
}

PSM_EXPORT int PsmKeyText(PsmKey key, char* out, int outLen)
{
    if (!out || outLen <= 0) return 0;
    psm::Settings::KeyText({key.vk, static_cast<uint8_t>(key.mods & 7)}, out, static_cast<size_t>(outLen));
    return 1;
}

PSM_EXPORT int PsmPadText(PsmPad pad, char* out, int outLen)
{
    if (!out || outLen <= 0) return 0;
    psm::Settings::PadText({pad.hold, pad.press}, out, static_cast<size_t>(outLen));
    return 1;
}

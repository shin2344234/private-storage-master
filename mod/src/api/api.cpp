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
#include "storage/stacks.h"
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

    void FromC(const PsmSettings* in, Values& v)
    {
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
    return psm::Settings::Update([in](Values& v) { FromC(in, v); }, why, why && whyLen > 0 ? static_cast<size_t>(whyLen) : 0) ? 1 : 0;
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
    const auto change = [in](Values& v)
    {
        v.hideKeysWithModifier = in->on != 0;
        v.hideKeysToggleKey = {in->toggleKey.vk, static_cast<uint8_t>(in->toggleKey.mods & 7)};
    };
    return psm::Settings::Update(change, why, why && whyLen > 0 ? static_cast<size_t>(whyLen) : 0) ? 1 : 0;
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
    const auto change = [in](Values& v)
    {
        v.autoStore = in->enabled != 0;
        for (int i = 0; i < PSM_STORAGES; ++i) v.autoStoreTo[i] = in->storages[i] != 0;
        v.autoStoreOnlyGained = in->onlyGained != 0;
    };
    return psm::Settings::Update(change, why, why && whyLen > 0 ? static_cast<size_t>(whyLen) : 0) ? 1 : 0;
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
    const auto change = [&sorted, n](Values& v)
    {
        memcpy(v.autoStoreNeverMove, sorted, sizeof(uint16_t) * n);
        v.autoStoreNeverMoveCount = n;
    };
    return psm::Settings::Update(change, why, why && whyLen > 0 ? static_cast<size_t>(whyLen) : 0) ? 1 : 0;
}

PSM_EXPORT int PsmDeposit(uint16_t item, int64_t gained) { return psm::deposit::Queue(item, gained) ? 1 : 0; }

PSM_EXPORT int PsmFreePlay(void) { return psm::deposit::FreePlayNow() ? 1 : 0; }

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

// ---------------------------------------------------------------- stack sizes
// include/stack_api.h, the interface Master Stack exports under the same names,
// so Master Looter's Stacks tab can talk to whichever of the two is installed.
#define STACK_API __declspec(dllexport)
#include "stack_api.h"

static_assert(STACK_STANDDOWN_NONE == psm::stacks::kApplying && STACK_STANDDOWN_OFF == psm::stacks::kOff &&
              STACK_STANDDOWN_OTHER_MOD == psm::stacks::kOtherMod && STACK_STANDDOWN_NO_ANCHOR == psm::stacks::kNoAnchor &&
              STACK_STANDDOWN_HOOK_FAILED == psm::stacks::kHookFailed && STACK_STANDDOWN_TOO_LATE == psm::stacks::kTooLate,
              "the STACK_STANDDOWN_ codes and stacks::Reason must match");
static_assert(STACK_MAX_MULTIPLIER == psm::Settings::kMaxStackMultiplier,
              "STACK_MAX_MULTIPLIER and Settings::kMaxStackMultiplier must match");

#define STACK_EXPORT extern "C" STACK_API

STACK_EXPORT int StackApiVersion(void) { return STACK_API_VERSION; }

STACK_EXPORT int StackGetStatus(StackStatus* out)
{
    // Any size from the first published layout upward, so a caller built against
    // an older header keeps working: it is a prefix of this struct, it gets the
    // fields it knows, and `size` comes back saying how many bytes were written.
    if (!out || out->size < static_cast<uint32_t>(STACK_STATUS_V1)) return 0;
    const uint32_t want = out->size < sizeof(StackStatus) ? out->size : static_cast<uint32_t>(sizeof(StackStatus));
    const psm::stacks::Report r = psm::stacks::Status();
    StackStatus full;
    StackStatus* const fill = &full;
    memset(fill, 0, sizeof full);
    fill->size = want;
    snprintf(fill->provider, sizeof fill->provider, "%s", PSM_NAME);
    snprintf(fill->providerModule, sizeof fill->providerModule, "%s", PSM_MODULE);
    snprintf(fill->version, sizeof fill->version, "%s", PSM_VERSION);
    snprintf(fill->gameVersion, sizeof fill->gameVersion, "%s", PSM_GAME);
    fill->applying = r.reason == psm::stacks::kApplying;
    fill->standDownReason = r.reason;
    fill->hooked = r.hooked;
    fill->multiplier = r.multiplier;
    fill->multiplierSetting = psm::Settings::Get().stackMultiplier;
    // Against what is in force, not against what the launch started with: a live
    // raise makes the two agree, and then no restart is needed.
    fill->restartNeeded = fill->multiplierSetting != fill->multiplier;
    fill->itemsRaised = r.patched;
    fill->itemsUnstackable = r.unstackable;
    fill->ceiling = STACK_CEILING;
    fill->maxMultiplier = STACK_MAX_MULTIPLIER;
    fill->liveRaise = psm::stacks::CanRaiseNow();
    fill->biggest = r.biggest;
    memcpy(out, fill, want);
    return 1;
}

STACK_EXPORT int StackStandDownText(int reason, char* out, int outLen)
{
    if (!out || outLen <= 0) return 0;
    switch (reason)
    {
    case STACK_STANDDOWN_NONE:
        snprintf(out, static_cast<size_t>(outLen), "%s is setting the stack sizes.", PSM_NAME);
        return 1;
    case STACK_STANDDOWN_OFF:
        snprintf(out, static_cast<size_t>(outLen), "Installed, but switched off: stacks hold what the game gives them.");
        return 1;
    case STACK_STANDDOWN_OTHER_MOD:
        snprintf(out, static_cast<size_t>(outLen), "Master Stack is installed and sets the stack sizes instead. Change it there.");
        return 1;
    case STACK_STANDDOWN_NO_ANCHOR:
        snprintf(out, static_cast<size_t>(outLen), "This game version keeps its item table somewhere the mod does not recognise, so stacks are "
                                                  "left alone. An update to %s is needed.", PSM_NAME);
        return 1;
    case STACK_STANDDOWN_HOOK_FAILED:
        snprintf(out, static_cast<size_t>(outLen), "The item table could not be hooked, so stacks are left alone. The log says why.");
        return 1;
    case STACK_STANDDOWN_TOO_LATE:
        snprintf(out, static_cast<size_t>(outLen), "The game read its item table before the mod started, so stacks are the game's own this "
                                                  "session. A restart fixes it.");
        return 1;
    default:
        snprintf(out, static_cast<size_t>(outLen), "Stacks are not being changed, and this version does not know why (reason %d).", reason);
        return 0;
    }
}

STACK_EXPORT int StackGetMultiplier(void) { return psm::Settings::Get().stackMultiplier; }

STACK_EXPORT int StackApplyMultiplier(int multiplier, char* why, int whyLen)
{
    if (why && whyLen > 0) why[0] = 0;
    if (multiplier < 1 || multiplier > STACK_MAX_MULTIPLIER)
    {
        if (why && whyLen > 0) snprintf(why, static_cast<size_t>(whyLen), "the multiplier has to be between 1 and %d", STACK_MAX_MULTIPLIER);
        return 0;
    }
    const auto change = [multiplier](Values& v) { v.stackMultiplier = multiplier; };
    if (!psm::Settings::Update(change, why, why && whyLen > 0 ? static_cast<size_t>(whyLen) : 0)) return 0;
    // Raising can take hold now. Anything else waits for the next launch, which
    // the caller sees as restartNeeded on its next status read. A refusal there is
    // not a failure of this call: the setting is saved either way.
    char note[192];
    if (!psm::stacks::RaiseNow(multiplier, note, sizeof note)) LOG("[stacks] x%d is saved for the next launch: %s", multiplier, note);
    return 1;
}

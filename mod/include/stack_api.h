/* Item stack sizes, the interface any mod that raises them exports.
 *
 * The same header ships with Private Storage Master and with Master Stack, and
 * both export exactly these names. A caller in the game process (Master
 * Looter's Stacks tab) finds a provider by looking for each module it knows
 * with GetModuleHandleW, then GetProcAddress on the functions below:
 *
 *     MasterStack.asi              the standalone mod
 *     PrivateStorageMaster.asi     the same feature inside the storage mod
 *
 * More than one may answer. Ask each for its status and edit the one whose
 * `applying` is 1: that is the mod actually changing the limits. Master Stack
 * takes precedence by design, so when it is installed Private Storage Master
 * reports applying 0 and leaves stacks alone. If nothing reports applying 1 the
 * feature is off everywhere, and StackApplyMultiplier on any provider that
 * answers turns it on from the next launch.
 *
 * Check StackApiVersion() first and use nothing if it differs from
 * STACK_API_VERSION. Every struct starts with its own size; set it before
 * passing one in, and a call with a size the provider does not know returns 0.
 *
 * Plain C, no allocation across the boundary, strings copied into the caller's
 * buffers. Everything is safe to call from any thread, every frame.
 */
#pragma once
#include <stdint.h>

#ifndef STACK_API
#define STACK_API   /* a provider defines it as dllexport while building */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define STACK_API_VERSION 1

/* The largest multiple a provider accepts, and the most any one stack holds. */
#define STACK_MAX_MULTIPLIER 1000
#define STACK_CEILING        999999

typedef struct StackStatus
{
    uint32_t size;               /* sizeof(StackStatus) */
    char     provider[24];       /* "Master Stack", "Private Storage Master" */
    char     providerModule[32]; /* "MasterStack.asi" */
    char     version[16];        /* the provider's own version */
    char     gameVersion[16];    /* game versions it was built for */
    int32_t  applying;           /* 1 = this mod is the one changing limits */
    int32_t  standDownReason;    /* STACK_STANDDOWN_*, 0 when applying */
    int32_t  hooked;             /* the item table reader is hooked */
    int32_t  multiplier;         /* in force this launch; 1 means untouched */
    int32_t  multiplierSetting;  /* in the ini, which may differ until a restart */
    int32_t  restartNeeded;      /* the setting differs from this launch */
    int32_t  itemsRaised;        /* item records whose limit was raised */
    int32_t  itemsUnstackable;   /* records left alone: the game does not stack them */
    /* This provider's own limits. Read them from here rather than from the
     * defines above, so a caller built against an older header still shows the
     * numbers the provider is really using. */
    int32_t  ceiling;            /* the most any one stack holds */
    int32_t  maxMultiplier;      /* the largest multiplier it accepts */
    int64_t  biggest;            /* the largest limit written */
} StackStatus;

#define STACK_STANDDOWN_NONE       0
#define STACK_STANDDOWN_OFF        1   /* the multiplier is 1 */
#define STACK_STANDDOWN_OTHER_MOD  2   /* another provider takes precedence */
#define STACK_STANDDOWN_NO_ANCHOR  3   /* the item table reader was not found */
#define STACK_STANDDOWN_HOOK_FAILED 4
#define STACK_STANDDOWN_TOO_LATE   5   /* the game read its item table first */

/* STACK_API_VERSION of this provider. Call it before anything else. */
STACK_API int StackApiVersion(void);

/* Fills out. Returns 1, or 0 when out is null or its size is unknown. */
STACK_API int StackGetStatus(StackStatus* out);

/* A sentence for a menu saying why stacks are not being changed, for a
 * standDownReason from StackGetStatus. It carries what a number cannot, such as
 * which mod took precedence. Writes something printable whenever out is valid,
 * and returns 1 for a code this provider knows, 0 for one it does not. */
STACK_API int StackStandDownText(int reason, char* out, int outLen);

/* The multiplier in the ini, 1 when off. 0 means the call failed. */
STACK_API int StackGetMultiplier(void);

/* Writes the multiplier to the provider's own ini, between 1 and the
 * maxMultiplier it reports, so a caller never touches a file itself. It takes
 * effect the next time the game starts, so a caller should say so. By the time
 * this returns 1, StackGetStatus reports the new multiplierSetting and the
 * matching restartNeeded. Returns 1 on success, or 0 with a reason in `why`
 * (which may be null). */
STACK_API int StackApplyMultiplier(int multiplier, char* why, int whyLen);

#ifdef __cplusplus
}
#endif

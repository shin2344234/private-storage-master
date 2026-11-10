#pragma once
#include <Windows.h>
#include <cstddef>
#include <cstdint>

// Diagnostic probes, compiled in only with CMake's PSM_DIAG option.
//
// Written for a report that two normal logs could not explain: on exe
// 1.0.0.3576, after a DMM update, the plugin started cleanly (every address
// resolved, all four hooks in, "[mod] ready") and then no storage key ever did
// anything, and the log said nothing more. Every path in the poller that drops a
// key press (the game is not the foreground window, PsmPauseInput is holding
// the keys, the modifiers do not match) is silent by design, so a normal log
// cannot tell those apart from "the key never reached the process". This module
// and the PSM_DIAG blocks in storage.cpp, pad.cpp, farhook.cpp and api.cpp say
// which one it is.
//
// Every function here is a cheap no-op in a normal build, so call sites need no
// #ifdef of their own unless they compute something only the probe uses.
namespace psm::diag
{
#ifdef PSM_DIAG
    constexpr bool kOn = true;
#else
    constexpr bool kOn = false;
#endif

    // Once, right after the settings are read: the build, the game exe's file
    // version and timestamp, Windows, elevation, the parent process (who
    // launched the game: Steam, DMM or something else), the ini's raw text and
    // every module loaded so far.
    void Startup();

    // Logs modules that loaded or unloaded since the last call. Poller thread.
    void WatchModules();

    // "pid 1234 CrimsonDesert.exe, class Root, 'Crimson Desert', visible, 3440x1440 at 0,0"
    void DescribeWindow(HWND h, char* out, size_t cap);
    // Image name of a process, "?" when it cannot be opened (an elevated one).
    void ProcessName(DWORD pid, char* out, size_t cap);
    // The module an address lies in, as "Name.dll+0x1234", or "?+0x...".
    void ModuleOf(const void* addr, char* out, size_t cap);

    // Stall watchdog for the poller thread. The poller calls Beat with a short
    // name of what it is about to do (a string literal, never freed). A thread
    // of this module logs when no beat has arrived for two seconds, naming the
    // last stage, and again when the poller comes back. A poller stuck inside
    // XInputGetState or a window call looks exactly like "no key works".
    void StartWatchdog();
    void Beat(const char* stage);
    void StopWatchdog();

    // PsmPauseInput bookkeeping: every call is counted with the module that
    // made it, so the heartbeat can say "paused 61 times in 5 s by
    // MasterLooter.asi" instead of only "paused".
    void NotePause(const void* caller, uint32_t ms);
    // Fills out with a summary of the calls since the last TakePauses, "" when
    // there were none, and resets the counters.
    void TakePauses(char* out, size_t cap);

    // A vectored handler that logs faults raised inside this plugin's own
    // code, or on the poller thread wherever they happen. First chance only:
    // the guarded readers in mem.cpp catch their own faults, and this handler
    // always passes the fault on. The first 40 are logged in full, then a
    // count every heartbeat.
    void InstallFaultLogger(DWORD pollerThreadId);
    void SetPollerThread(DWORD id);
    long FaultsLogged();
    void RemoveFaultLogger();
}

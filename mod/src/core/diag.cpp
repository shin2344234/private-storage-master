#include "core/diag.h"

#ifdef PSM_DIAG

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/paths.h"
#include "game/mem.h"
#include "version.h"

namespace psm::diag
{
    namespace
    {
        // ------------------------------------------------------------ helpers
        // Narrow copy of a wide string for the log, which is UTF-8 throughout.
        std::string Utf8(const wchar_t* w)
        {
            if (!w || !*w) return std::string();
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n <= 1) return std::string();
            std::string s(static_cast<size_t>(n - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
            return s;
        }

        // Just the file name of a path, for the module list.
        const wchar_t* BaseName(const wchar_t* path)
        {
            const wchar_t* b = path;
            for (const wchar_t* p = path; *p; ++p)
                if (*p == L'\\' || *p == L'/') b = p + 1;
            return b;
        }

        // A file's VS_FIXEDFILEINFO version as "a.b.c.d", or "" when it has none.
        void FileVersion(const wchar_t* path, char* out, size_t cap)
        {
            out[0] = 0;
            DWORD handle = 0;
            const DWORD size = GetFileVersionInfoSizeW(path, &handle);
            if (!size) return;
            std::vector<unsigned char> buf(size);
            if (!GetFileVersionInfoW(path, 0, size, buf.data())) return;
            VS_FIXEDFILEINFO* fi = nullptr;
            UINT len = 0;
            if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi || len < sizeof *fi) return;
            snprintf(out, cap, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                     HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        }

        // The PE header's link timestamp of a loaded module. Two exes with the
        // same version string but different stamps are different builds.
        DWORD LinkStamp(HMODULE m)
        {
            const auto base = reinterpret_cast<uintptr_t>(m);
            uint32_t peOff = 0, stamp = 0;
            if (!mem::Read32(base + 0x3C, &peOff)) return 0;
            if (!mem::Read32(base + peOff + 8, &stamp)) return 0;
            return stamp;
        }

        // Windows' real version: GetVersionEx lies to an exe without the
        // right manifest, RtlGetVersion does not.
        void WindowsVersion(char* out, size_t cap)
        {
            using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
            OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof vi;
            const auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
            if (fn && fn(&vi) == 0) snprintf(out, cap, "%lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
            else snprintf(out, cap, "?");
        }

        // Whether this process runs elevated. A game started elevated by a mod
        // manager cannot get keyboard state from a non-elevated foreground window
        // and vice versa (UIPI), which is one way keys can go missing.
        const char* Elevation()
        {
            HANDLE tok = nullptr;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return "unknown";
            TOKEN_ELEVATION e{};
            DWORD len = 0;
            const bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &len) != 0;
            CloseHandle(tok);
            if (!ok) return "unknown";
            return e.TokenIsElevated ? "elevated (run as administrator)" : "not elevated";
        }

        // The process that started this one and its own parent, which says
        // whether the game came from Steam, from DMM, or from something else.
        void LogParents()
        {
            DWORD pid = GetCurrentProcessId();
            for (int depth = 0; depth < 3; ++depth)
            {
                DWORD parent = 0;
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap == INVALID_HANDLE_VALUE) return;
                PROCESSENTRY32W pe{};
                pe.dwSize = sizeof pe;
                for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
                    if (pe.th32ProcessID == pid) { parent = pe.th32ParentProcessID; break; }
                CloseHandle(snap);
                if (!parent) return;
                char name[MAX_PATH];
                ProcessName(parent, name, sizeof name);
                LOG_NOTE("[diag] %s process: pid %lu %s", depth == 0 ? "parent" : "grandparent", parent, name);
                pid = parent;
                if (depth >= 1) return;
            }
        }

        // The ini as it is on disk, line by line. The DMM update deleted it once
        // already; this shows what the plugin actually read.
        void LogIni()
        {
            const std::wstring path = Paths::File(PSM_INI);
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
            {
                LOG_NOTE("[diag] ini %s does not exist (error %lu)", Utf8(path.c_str()).c_str(), GetLastError());
                return;
            }
            SYSTEMTIME st{}, lt{};
            FileTimeToSystemTime(&fa.ftLastWriteTime, &st);
            SystemTimeToTzSpecificLocalTime(nullptr, &st, &lt);
            LOG_NOTE("[diag] ini %s, %lu bytes, last written %04u-%02u-%02u %02u:%02u:%02u%s", Utf8(path.c_str()).c_str(),
                     fa.nFileSizeLow, lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond,
                     (fa.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? ", read-only" : "");
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) return;
            char line[512];
            int n = 0;
            while (fgets(line, sizeof line, f) && n < 200)
            {
                size_t l = strlen(line);
                while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
                // Comments are the plugin's own help text; only settings matter.
                if (!l || line[0] == ';' || line[0] == '#') continue;
                LOG_NOTE("[diag] ini| %s", line);
                ++n;
            }
            fclose(f);
        }

        // ------------------------------------------------------------ modules
        struct Mod { HMODULE h; std::wstring path; };
        std::vector<Mod> g_mods;
        std::mutex g_modsMu;

        void ListModules(std::vector<Mod>& out)
        {
            out.clear();
            HMODULE hs[1024];
            DWORD need = 0;
            if (!EnumProcessModules(GetCurrentProcess(), hs, sizeof hs, &need)) return;
            const DWORD n = need / sizeof(HMODULE);
            for (DWORD i = 0; i < n && i < 1024; ++i)
            {
                wchar_t p[MAX_PATH];
                if (!GetModuleFileNameW(hs[i], p, MAX_PATH)) continue;
                out.push_back(Mod{hs[i], p});
            }
        }

        // A module outside the Windows folder: the game's own DLLs, mods, ASI
        // loaders, overlays, injectors. Those are the interesting ones, and the
        // full list is a hundred-odd system DLLs otherwise.
        bool NotSystem(const std::wstring& path)
        {
            wchar_t win[MAX_PATH];
            const UINT n = GetWindowsDirectoryW(win, MAX_PATH);
            return !(n && _wcsnicmp(path.c_str(), win, n) == 0);
        }

        void LogModule(const char* what, const Mod& m)
        {
            char ver[64];
            FileVersion(m.path.c_str(), ver, sizeof ver);
            MODULEINFO mi{};
            GetModuleInformation(GetCurrentProcess(), m.h, &mi, sizeof mi);
            LOG_NOTE("[diag] module %s %s at %p, %lu bytes%s%s", what, Utf8(m.path.c_str()).c_str(), static_cast<void*>(m.h),
                     mi.SizeOfImage, ver[0] ? ", version " : "", ver);
        }

        // ------------------------------------------------------------ watchdog
        std::atomic<const char*> g_stage{"start"};
        std::atomic<DWORD> g_beatAt{0};
        std::atomic<bool> g_wdStop{false};
        HANDLE g_wd = nullptr;

        DWORD WINAPI Watchdog(LPVOID)
        {
            bool stalled = false;
            DWORD stalledSince = 0;
            while (!g_wdStop.load())
            {
                Sleep(500);
                const DWORD at = g_beatAt.load();
                if (!at) continue;   // the poller has not started yet
                const DWORD age = GetTickCount() - at;
                if (!stalled && age > 2000)
                {
                    stalled = true;
                    stalledSince = at;
                    LOG_ERR("[diag] the key poller has been stuck for %lu ms in '%s'. While it is stuck no key or combo can act.", age,
                            g_stage.load());
                }
                else if (stalled && age < 1000)
                {
                    stalled = false;
                    LOG_NOTE("[diag] the key poller is running again after %lu ms", GetTickCount() - stalledSince);
                }
            }
            return 0;
        }

        // ------------------------------------------------------------ pauses
        std::mutex g_pauseMu;
        struct PauseCaller { const void* module; char name[96]; unsigned calls; uint32_t lastMs; };
        PauseCaller g_pausers[8] = {};
        int g_pauserCount = 0;
        unsigned g_pauseTotal = 0;

        // ------------------------------------------------------------ faults
        PVOID g_veh = nullptr;
        std::atomic<DWORD> g_pollerTid{0};
        std::atomic<long> g_faults{0};
        constexpr long kFaultsInFull = 40;

        bool InOurModule(const void* p)
        {
            HMODULE m = nullptr;
            return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCWSTR>(p), &m) &&
                   m == Paths::Module();
        }

        LONG CALLBACK FaultLogger(EXCEPTION_POINTERS* ep)
        {
            const EXCEPTION_RECORD* r = ep->ExceptionRecord;
            const DWORD code = r->ExceptionCode;
            // Only real faults. Debug strings, thread names, C++ throws and the
            // like are not what this is for. A stack overflow leaves no stack to
            // log with, so it is left alone too.
            if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
                code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR)
                return EXCEPTION_CONTINUE_SEARCH;
            const bool ours = InOurModule(r->ExceptionAddress);
            const bool poller = GetCurrentThreadId() == g_pollerTid.load();
            if (!ours && !poller) return EXCEPTION_CONTINUE_SEARCH;
            const long n = ++g_faults;
            if (n > kFaultsInFull) return EXCEPTION_CONTINUE_SEARCH;
            char where[160];
            ModuleOf(r->ExceptionAddress, where, sizeof where);
            const uintptr_t info0 = r->NumberParameters > 0 ? r->ExceptionInformation[0] : 0;
            const uintptr_t info1 = r->NumberParameters > 1 ? r->ExceptionInformation[1] : 0;
            // Many of these are expected: mem.cpp's guarded readers probe game
            // pointers and catch the fault. The log says first chance for that
            // reason, and a crash would show up as the game closing after it.
            LOG("[diag] first-chance fault %08lX at %s on %s thread %lu (%s address %p)%s", code, where, poller ? "the poller" : "a",
                GetCurrentThreadId(), info0 == 0 ? "reading" : info0 == 1 ? "writing" : info0 == 8 ? "executing" : "touching",
                reinterpret_cast<void*>(info1), n == kFaultsInFull ? "; further faults are only counted" : "");
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    // ------------------------------------------------------------ public
    void DescribeWindow(HWND h, char* out, size_t cap)
    {
        if (!h) { snprintf(out, cap, "no window"); return; }
        DWORD pid = 0;
        const DWORD tid = GetWindowThreadProcessId(h, &pid);
        char proc[MAX_PATH], cls[128] = {}, title[128] = {};
        ProcessName(pid, proc, sizeof proc);
        GetClassNameA(h, cls, sizeof cls);
        // The title only for the game's own windows. Another program's title is
        // the player's business (a browser tab, a chat), and these logs get
        // pasted into public channels; its exe name and class say enough.
        // WM_GETTEXT is sent with a timeout, since the game's window thread may
        // be busy and an unbounded SendMessage from the poller would stall it.
        if (pid == GetCurrentProcessId())
        {
            wchar_t wt[128] = {};
            DWORD_PTR got = 0;
            SendMessageTimeoutW(h, WM_GETTEXT, 128, reinterpret_cast<LPARAM>(wt), SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &got);
            snprintf(title, sizeof title, "%s", Utf8(wt).c_str());
        }
        else snprintf(title, sizeof title, "(another program's title is not logged)");
        RECT rc{};
        GetWindowRect(h, &rc);
        snprintf(out, cap, "%p pid %lu tid %lu %s%s, class %s, '%s', %s%s, %ldx%ld at %ld,%ld", static_cast<void*>(h), pid, tid, proc,
                 pid == GetCurrentProcessId() ? " (the game)" : "", cls, title, IsWindowVisible(h) ? "visible" : "hidden",
                 IsIconic(h) ? ", minimised" : "", rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top);
    }

    void ProcessName(DWORD pid, char* out, size_t cap)
    {
        snprintf(out, cap, "?");
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!p)
        {
            snprintf(out, cap, "? (cannot open pid %lu, error %lu: elevated or protected)", pid, GetLastError());
            return;
        }
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) snprintf(out, cap, "%s", Utf8(BaseName(path)).c_str());
        CloseHandle(p);
    }

    void ModuleOf(const void* addr, char* out, size_t cap)
    {
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(addr), &m) ||
            !m)
        {
            snprintf(out, cap, "%p (no module: allocated code, a trampoline or a hook)", addr);
            return;
        }
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(m, p, MAX_PATH);
        snprintf(out, cap, "%s+0x%llX", Utf8(BaseName(p)).c_str(),
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m)));
    }

    void Startup()
    {
        LOG_NOTE("[diag] DIAGNOSTIC BUILD %s%s, built %s %s. DebugLog is forced on whatever the ini says, and old logs are kept.",
                 PSM_VERSION, PSM_BUILD_TAG, __DATE__, __TIME__);

        // The game exe: path, file version, size, link stamp. The [mod] line's
        // range is only what this build was written for; this is what is running.
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        char ver[64];
        FileVersion(exe, ver, sizeof ver);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        GetFileAttributesExW(exe, GetFileExInfoStandard, &fa);
        LOG_NOTE("[diag] game exe %s, file version %s, %llu bytes on disk, link stamp %08lX, image %zu bytes in memory",
                 Utf8(exe).c_str(), ver[0] ? ver : "?",
                 (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow, LinkStamp(GetModuleHandleW(nullptr)),
                 mem::Game().size);

        wchar_t self[MAX_PATH];
        GetModuleFileNameW(Paths::Module(), self, MAX_PATH);
        LOG_NOTE("[diag] plugin %s at %p", Utf8(self).c_str(), static_cast<void*>(Paths::Module()));

        char win[64];
        WindowsVersion(win, sizeof win);
        LOG_NOTE("[diag] Windows %s, process %lu %s, keyboard layout %p", win, GetCurrentProcessId(), Elevation(),
                 static_cast<void*>(GetKeyboardLayout(0)));
        LogParents();
        LogIni();

        // Everything loaded so far that is not part of Windows. A mod manager's
        // runtime (DmmRuntime.asi on the reporter's machine), ASI loaders,
        // overlays and other mods all show up here, with their versions.
        std::lock_guard<std::mutex> lk(g_modsMu);
        ListModules(g_mods);
        int shown = 0, sys = 0;
        for (const Mod& m : g_mods)
        {
            if (NotSystem(m.path)) { LogModule("loaded", m); ++shown; }
            else ++sys;
        }
        LOG_NOTE("[diag] %d modules outside Windows listed above, %d Windows modules not listed", shown, sys);
    }

    void WatchModules()
    {
        std::vector<Mod> now;
        ListModules(now);
        std::lock_guard<std::mutex> lk(g_modsMu);
        for (const Mod& m : now)
        {
            bool known = false;
            for (const Mod& o : g_mods) known |= o.h == m.h && o.path == m.path;
            // A late load is worth logging even from Windows: an input or
            // overlay DLL arriving mid-session is exactly what to look for.
            if (!known) LogModule("loaded later:", m);
        }
        for (const Mod& o : g_mods)
        {
            bool still = false;
            for (const Mod& m : now) still |= o.h == m.h && o.path == m.path;
            if (!still) LogModule("unloaded:", o);
        }
        g_mods.swap(now);
    }

    void StartWatchdog()
    {
        g_wdStop = false;
        g_wd = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    }

    void Beat(const char* stage)
    {
        g_stage.store(stage, std::memory_order_relaxed);
        g_beatAt.store(GetTickCount(), std::memory_order_relaxed);
    }

    void StopWatchdog()
    {
        g_wdStop = true;
        if (g_wd)
        {
            WaitForSingleObject(g_wd, 1000);
            CloseHandle(g_wd);
            g_wd = nullptr;
        }
    }

    void NotePause(const void* caller, uint32_t ms)
    {
        HMODULE m = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(caller), &m);
        std::lock_guard<std::mutex> lk(g_pauseMu);
        ++g_pauseTotal;
        for (int i = 0; i < g_pauserCount; ++i)
            if (g_pausers[i].module == m) { ++g_pausers[i].calls; g_pausers[i].lastMs = ms; return; }
        if (g_pauserCount >= 8) return;
        PauseCaller& c = g_pausers[g_pauserCount++];
        c.module = m;
        c.calls = 1;
        c.lastMs = ms;
        ModuleOf(caller, c.name, sizeof c.name);
        // The first call from each module is a note of its own, so even a
        // short session shows who pauses the keys.
        LOG_NOTE("[diag] PsmPauseInput(%u) first called from %s", ms, c.name);
    }

    void TakePauses(char* out, size_t cap)
    {
        out[0] = 0;
        std::lock_guard<std::mutex> lk(g_pauseMu);
        if (!g_pauseTotal) return;
        size_t at = static_cast<size_t>(snprintf(out, cap, "%u PsmPauseInput calls:", g_pauseTotal));
        for (int i = 0; i < g_pauserCount && at < cap; ++i)
        {
            if (!g_pausers[i].calls) continue;
            at += static_cast<size_t>(snprintf(out + at, cap - at, " %u from %s (last %u ms)", g_pausers[i].calls, g_pausers[i].name,
                                               g_pausers[i].lastMs));
            g_pausers[i].calls = 0;
        }
        g_pauseTotal = 0;
    }

    void InstallFaultLogger(DWORD pollerThreadId)
    {
        g_pollerTid = pollerThreadId;
        // Last in the chain (0): every other handler, the game's own
        // self-checking one included, sees the fault first.
        if (!g_veh) g_veh = AddVectoredExceptionHandler(0, FaultLogger);
    }
    void SetPollerThread(DWORD id) { g_pollerTid = id; }
    long FaultsLogged() { return g_faults.load(); }
    void RemoveFaultLogger()
    {
        if (g_veh) RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
    }
}

#else   // not a diagnostic build: every probe is empty

namespace psm::diag
{
    void Startup() {}
    void WatchModules() {}
    void DescribeWindow(HWND, char* out, size_t cap) { if (cap) out[0] = 0; }
    void ProcessName(DWORD, char* out, size_t cap) { if (cap) out[0] = 0; }
    void ModuleOf(const void*, char* out, size_t cap) { if (cap) out[0] = 0; }
    void StartWatchdog() {}
    void Beat(const char*) {}
    void StopWatchdog() {}
    void NotePause(const void*, uint32_t) {}
    void TakePauses(char* out, size_t cap) { if (cap) out[0] = 0; }
    void InstallFaultLogger(DWORD) {}
    void SetPollerThread(DWORD) {}
    long FaultsLogged() { return 0; }
    void RemoveFaultLogger() {}
}

#endif

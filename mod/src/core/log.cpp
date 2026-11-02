#include "core/log.h"

#include <Windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

#include "core/paths.h"

namespace psm::Log
{
    static std::mutex               g_mu;
    static std::deque<std::string>  g_recent;   // for the Status tab
    static std::deque<std::string>  g_pending;  // not yet on disk
    static FILE*                    g_file    = nullptr;
    static bool                     g_claimed = false;
    static constexpr size_t         kKeep     = 400;
    static std::atomic<bool>        g_debug{true};

    static std::string              g_lastError;

    void SetDebug(bool on) { g_debug.store(on); }

    void LastError(char* out, size_t cap)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        snprintf(out, cap, "%s", g_lastError.c_str());
    }
    bool Debug() { return g_debug.load(); }

    static std::string Stamp()
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char b[32];
        snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        return b;
    }

    void Write(const char* level, const char* fmt, ...)
    {
        char msg[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);

        std::string line = "[" + Stamp() + "] [" + level + "] " + msg;

        std::lock_guard<std::mutex> lk(g_mu);
        if (level[0] == 'e') g_lastError = msg;
        g_recent.push_back(line);
        if (g_recent.size() > kKeep) g_recent.pop_front();
        if (g_file)
        {
            fputs(line.c_str(), g_file);
            fputc('\n', g_file);
            fflush(g_file);
        }
        else
        {
            g_pending.push_back(line);
            if (g_pending.size() > kKeep) g_pending.pop_front();
        }
    }

    // Keep the last five sessions as well as the live one. A report needs the
    // live log and .01, and a probe run is usually worth comparing against the
    // one before it. Lifted from Master Looter, where a launch that destroyed
    // the previous log cost a capture that had answered something.
    //
    // Builds before November 2026 kept 24, and players noticed the pile in
    // bin64. rspn670 asked on Nexus why the mod makes countless .log files, and
    // sh3riff counted 77. Rotate deletes .06 up to .24 as it goes, so a folder
    // an older build filled is down to five on the first launch.
    //
    // Plain text, not compressed. These get pasted into a message, and an
    // archive is a barrier to that.
    static constexpr int kArchives = 5;      // plus the live one
    static constexpr int kOldArchives = 24;  // the most any earlier build kept

    static void Rotate(const wchar_t* base)
    {
        wchar_t from[96], to[96], live[96];
        _snwprintf_s(live, _countof(live), _TRUNCATE, L"%s.log", base);
        // Oldest out first, then each one shuffles up a place, so the numbers
        // read as age: 01 is the session before this one, 05 the furthest back.
        // Everything past the oldest kept one goes, back to the 24 that earlier
        // builds kept.
        for (int i = kOldArchives; i >= kArchives; --i)
        {
            _snwprintf_s(to, _countof(to), _TRUNCATE, L"%s.%02d.log", base, i);
            DeleteFileW(Paths::File(to).c_str());
        }
        for (int i = kArchives - 1; i >= 1; --i)
        {
            _snwprintf_s(from, _countof(from), _TRUNCATE, L"%s.%02d.log", base, i);
            _snwprintf_s(to,   _countof(to),   _TRUNCATE, L"%s.%02d.log", base, i + 1);
            MoveFileExW(Paths::File(from).c_str(), Paths::File(to).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        _snwprintf_s(to, _countof(to), _TRUNCATE, L"%s.01.log", base);
        MoveFileExW(Paths::File(live).c_str(), Paths::File(to).c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    void Prune(const wchar_t* base, int keep)
    {
        wchar_t name[96];
        for (int i = keep + 1; i <= kOldArchives; ++i)
        {
            _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s.%02d.log", base, i);
            DeleteFileW(Paths::File(name).c_str());
        }
    }

    void Claim(const wchar_t* base)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_claimed) return;
        g_claimed = true;
        Rotate(base);
        wchar_t live[96];
        _snwprintf_s(live, _countof(live), _TRUNCATE, L"%s.log", base);
        g_file = _wfopen(Paths::File(live).c_str(), L"w");
        if (!g_file) return;
        for (const auto& l : g_pending)
        {
            fputs(l.c_str(), g_file);
            fputc('\n', g_file);
        }
        g_pending.clear();
        fflush(g_file);
    }

    bool Claimed()
    {
        std::lock_guard<std::mutex> lk(g_mu);
        return g_claimed;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_file) { fclose(g_file); g_file = nullptr; }
    }

    void Snapshot(std::vector<std::string>& out, int maxLines)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        out.clear();
        const size_t n = g_recent.size();
        const size_t start = (maxLines > 0 && n > static_cast<size_t>(maxLines)) ? n - maxLines : 0;
        for (size_t i = start; i < n; ++i) out.push_back(g_recent[i]);
    }
}

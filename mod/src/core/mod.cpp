#include "core/mod.h"

#include <atomic>
#include <cwchar>

#include "core/log.h"
#include "core/paths.h"
#include "game/mem.h"
#include "probe/probe.h"
#include "version.h"

namespace
{
    std::atomic<bool> g_stop{false};
    HANDLE g_thread = nullptr;

    DWORD WINAPI Worker(LPVOID)
    {
        LOG("[mod] %s %s (%s), game image at 0x%p, %zu bytes", PSM_NAME, PSM_VERSION, PSM_BUILD,
            reinterpret_cast<void*>(psm::mem::Game().base), psm::mem::Game().size);

        // Hooks go in once the game has had time to build its UI objects. The
        // probe only reads, but installing against a half-loaded image is how a
        // resolver meets code that is not unpacked yet.
        for (int i = 0; i < 20 && !g_stop.load(); ++i) Sleep(500);
        if (g_stop.load()) return 0;
        psm::probe::Start();
        while (!g_stop.load()) Sleep(250);
        LOG("[mod] worker stopped");
        return 0;
    }
}

namespace psm::Mod
{
    // crashpad_handler.exe loads ASI plugins too (671,744-byte image on this
    // build). That instance names its own log and touches nothing.
    static constexpr size_t kMinGameImage = 64ull * 1024 * 1024;

    void Initialize(HMODULE module)
    {
        Paths::Init(module);
        const size_t size = mem::Game().size;
        if (!mem::Game().base || size < kMinGameImage)
        {
            wchar_t name[64];
            _snwprintf_s(name, _countof(name), _TRUNCATE, L"%s.other-%lu", PSM_FILEBASE, GetCurrentProcessId());
            Log::Claim(name);
            LOG("[mod] this process has a %zu byte image, which is not the game, so nothing is changed here.", size);
            Log::Shutdown();
            return;
        }
        Log::Claim(PSM_FILEBASE);
        g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }

    void Shutdown(bool processExiting)
    {
        g_stop.store(true);
        if (processExiting)
        {
            Log::Shutdown();
            return;
        }
        if (g_thread)
        {
            WaitForSingleObject(g_thread, 3000);
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
        probe::Stop();
        Log::Shutdown();
    }
}

#include "core/mod.h"

#include <atomic>

#include "core/log.h"
#include "core/paths.h"
#include "core/settings.h"
#include "game/farhook.h"
#include "game/mem.h"
#include "storage/capacity.h"
#include "storage/stacks.h"
#include "storage/storage.h"
#include "version.h"

namespace
{
    std::atomic<bool> g_stop{false};
    HANDLE g_thread = nullptr;
    bool g_storageStarted = false;

    DWORD WINAPI Worker(LPVOID)
    {
        psm::Settings::Load();
        // Twenty-four archived sessions are for chasing a problem. Without
        // DebugLog a session is a handful of lines, and the last two are all a
        // report needs, including the one before a crash and a relaunch.
        if (!psm::Log::Debug()) psm::Log::Prune(PSM_FILEBASE, 2);
        LOG_NOTE("[mod] %s %s for Crimson Desert %s, game image at 0x%p, %zu bytes", PSM_NAME, PSM_VERSION, PSM_GAME,
                 reinterpret_cast<void*>(psm::mem::Game().base), psm::mem::Game().size);
        if (!psm::Settings::Get().enabled)
        {
            LOG_NOTE("[mod] Enabled=0, so the mod does nothing");
            return 0;
        }
        if (GetModuleHandleW(L"PrivateStorageAnywhere.asi"))
            LOG_ERR("[mod] PrivateStorageAnywhere.asi is also loaded. The two change the same parts of the game; remove that one.");

        const DWORD started = GetTickCount();
        // Storage sizes first: they only reach storage built after the hook is in.
        psm::capacity::Start();
        // The same for stack sizes: the item table is read once, early.
        psm::stacks::Start();

        // Opening storage waits until the game has built its UI objects.
        while (!g_stop.load() && GetTickCount() - started < 10000) Sleep(250);
        if (g_stop.load()) return 0;
        psm::stacks::Flush();
        g_storageStarted = psm::storage::Start();
        LOG_NOTE("[mod] %s", g_storageStarted ? "ready" : "storage keys are off; see the errors above");
        return 0;
    }
}

namespace psm::Mod
{
    // crashpad_handler.exe loads ASI plugins too (671,744-byte image on this
    // build). That instance does nothing at all.
    static constexpr size_t kMinGameImage = 64ull * 1024 * 1024;

    void Initialize(HMODULE module)
    {
        Paths::Init(module);
        const size_t size = mem::Game().size;
        // It writes no log: one file per crash handler launch piled up beside the plugin.
        if (!mem::Game().base || size < kMinGameImage) return;
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
        if (g_storageStarted) storage::Stop();
        stacks::Stop();
        capacity::Stop();
        farhook::RemoveAll();
        Log::Shutdown();
    }
}

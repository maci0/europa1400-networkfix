/*
 * main.c: DLL entry point for the Europa 1400 network fix. Hook setup runs on
 * its own thread because doing that work inside DllMain risks a loader-lock
 * deadlock.
 */

#define WIN32_LEAN_AND_MEAN
#include "hooks.h"
#include "logging.h"
#include <windows.h>

// Upper bound on waiting for the init thread during a real FreeLibrary unload;
// cleanup proceeds regardless so a stuck init cannot wedge the unload.
#define INIT_THREAD_JOIN_TIMEOUT_MS 10000

// Global module handle for configuration access
HMODULE       g_hModule = NULL;
static HANDLE g_hInitThread = NULL;

/* Hook setup, off the loader lock (see file header). Failure is non-fatal: the
 * game runs on unpatched. Nothing reads the exit code. */
static DWORD WINAPI init_thread(LPVOID lpParam)
{
    (void)lpParam;
    if (!init_hooks())
    {
        log_msg("[HOOK] Hook initialization failed; game will run without network fix");
        OutputDebugStringA("[HOOK] Hook initialization failed; running without fix.\n");
        return 1;
    }

    return 0;
}

// cppcheck-suppress unusedFunction
// False positive: DllMain is the PE entry point; only the loader calls it.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;

        DisableThreadLibraryCalls(hModule);

        if (!init_logging(hModule))
        {
            OutputDebugStringA("[HOOK] Failed to initialize logging. Aborting attach.\n");
            return FALSE;
        }

        g_hInitThread = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (!g_hInitThread)
        {
            log_msg("[HOOK] Failed to create initialization thread");
            close_logging();
            return FALSE;
        }
        // Keep handle for join on detach; don't close here
        break;

    case DLL_PROCESS_DETACH:
        // lpReserved != NULL means the process is terminating: the loader is
        // already tearing down other threads, so waiting or freeing here can
        // deadlock. Only join + cleanup on a real FreeLibrary unload.
        if (g_hInitThread)
        {
            if (lpReserved == NULL)
            {
                DWORD wait = WaitForSingleObject(g_hInitThread, INIT_THREAD_JOIN_TIMEOUT_MS);
                if (wait != WAIT_OBJECT_0)
                {
                    // The init thread may still be mid-initialization while we
                    // tear hooks down below; leave a trace instead of failing
                    // silently (WAIT_TIMEOUT = 0x102, WAIT_FAILED = 0xFFFFFFFF).
                    log_msg("[HOOK] Init thread join incomplete (wait result 0x%lX); "
                            "continuing cleanup",
                            (unsigned long)wait);
                }
            }
            CloseHandle(g_hInitThread);
            g_hInitThread = NULL;
        }
        if (lpReserved == NULL)
        {
            log_msg("[HOOK] DLL detaching from process");
            cleanup_hooks();
            close_logging();
        }
        break;
    }

    return TRUE;
}

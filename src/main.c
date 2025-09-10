/*
 * main.c: DLL entry point and initialization thread for Europa 1400 network fix.
 *
 * This file contains the DllMain entry point and manages the initialization
 * of logging and hook systems in a separate thread to avoid DllMain deadlocks.
 */

#define WIN32_LEAN_AND_MEAN
#include "hooks.h"
#include "logging.h"
#include <windows.h>

// Global module handle for configuration access
HMODULE g_hModule = NULL;

/**
 * Initialization thread procedure that sets up logging and hooks.
 * Runs in a separate thread to avoid potential DllMain deadlock issues.
 *
 * This thread:
 * 1. Initializes the hook system
 * 2. Reports initialization status
 * 3. Exits cleanly
 *
 * @param lpParam Unused thread parameter
 * @return 0 on success, 1 on failure
 */
static DWORD WINAPI init_thread(LPVOID lpParam)
{
    // Initialize hook system
    if (!init_hooks())
    {
        logf("[HOOK] Hook initialization failed");
        return 1;
    }

    return 0;
}

/**
 * DLL entry point called by Windows loader.
 * Handles process attach/detach and thread events.
 *
 * For process attach:
 * - Stores module handle for configuration access
 * - Disables thread library calls for performance
 * - Initializes logging system
 * - Creates initialization thread to setup hooks
 *
 * For process detach:
 * - Cleans up hooks and logging
 *
 * @param hModule Handle to DLL module
 * @param dwReason Reason for call (DLL_PROCESS_ATTACH, etc.)
 * @param lpReserved Reserved parameter
 * @return TRUE on success, FALSE on failure
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;

        // Disable thread library calls for performance using DisableThreadLibraryCalls()
        DisableThreadLibraryCalls(hModule);

        // Initialize logging system first
        if (!init_logging(hModule))
        {
            OutputDebugStringA("[HOOK] Failed to initialize logging. Aborting attach.\n");
            return FALSE;
        }

        // Create initialization thread to avoid DllMain deadlock issues
        // Uses CreateThread() and CloseHandle() for proper resource management
        HANDLE hThread = CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        if (hThread)
        {
            CloseHandle(hThread);
        }
        else
        {
            logf("[HOOK] Failed to create initialization thread");
            close_logging();
            return FALSE;
        }
        break;

    case DLL_PROCESS_DETACH:
        logf("[HOOK] DLL detaching from process");

        // Clean up hooks and logging
        cleanup_hooks();
        close_logging();
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        // No special handling needed for thread attach/detach
        break;
    }

    return TRUE;
}
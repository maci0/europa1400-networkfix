/*
 * hooks.c: Hook implementations and management for network stability fixes.
 *
 * This file contains the hook functions that intercept Windows API calls
 * and server.dll functions to add stability improvements.
 */

#define WIN32_LEAN_AND_MEAN
#include "hooks.h"
#include "MinHook.h"
#include "logging.h"
#include "sha256.h"
#include "versions.h"
#include <psapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define CALLER_IP() _ReturnAddress()
#elif defined(__clang__) || defined(__GNUC__)
#define CALLER_IP() __builtin_extract_return_addr(__builtin_return_address(0))
#else
#error Unsupported compiler
#endif

// Global state
static BOOL      g_HooksInitialized = false;
static uintptr_t g_ServerBase = 0;
static size_t    g_ServerSize = 0;
static DWORD     g_server_rva = 0;

// Original function pointers
static int(WSAAPI *real_recv)(SOCKET, char *, int, int) = NULL;
static int(WSAAPI *real_send)(SOCKET, const char *, int, int) = NULL;
static DWORD(WINAPI *real_GetTickCount)(void) = NULL;

/* Server.dll target function - RVA varies by version */
typedef int(__cdecl *pServerFunction_t)(int *ctx, int received, int totalLen);
static pServerFunction_t real_ServerFunction = NULL;

/**
 * Detect server.dll version by calculating its SHA256 hash.
 * Returns the RVA offset for the target function, or 0 if unknown version.
 */
static DWORD detect_server_version(const char *serverPath)
{
    // Convert path to wide char for Windows API
    wchar_t widePath[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, serverPath, -1, widePath, MAX_PATH) == 0)
    {
        logf("[HOOK] Failed to convert server path to wide char");
        return 0;
    }

    // Calculate file hash
    char fileHash[65]; // 64 chars + null terminator
    if (!calculate_file_sha256(widePath, fileHash, sizeof(fileHash)))
    {
        logf("[HOOK] Failed to calculate SHA256 for %s", serverPath);
        return 0;
    }

    logf("[HOOK] server.dll SHA256: %s", fileHash);

    // Look up version in known list
    for (int i = 0; known_versions[i].sha256_hash != NULL; i++)
    {
        if (strcmp(fileHash, known_versions[i].sha256_hash) == 0)
        {
            logf("[HOOK] Detected %s version (RVA: 0x%X)", known_versions[i].version_name,
                 known_versions[i].target_rva);
            return known_versions[i].target_rva;
        }
    }

    logf("[HOOK] Unknown server.dll version with hash: %s", fileHash);
    return 0;
}

/**
 * Gets the number of bytes available to read from socket.
 */
static int get_available_bytes(SOCKET s)
{
    u_long available = 0;
    if (ioctlsocket(s, FIONREAD, &available) == SOCKET_ERROR)
    {
        return -1;
    }
    return (int)available;
}

/**
 * Initializes the g_ServerBase and g_ServerSize variables by locating server.dll
 * in the current process address space.
 *
 * Uses GetModuleHandleA() to find server.dll and GetModuleInformation() to
 * retrieve its base address and size for caller validation.
 *
 * @return TRUE if already initialized or successfully initialized, FALSE on error
 */
BOOL init_server_module_range(void)
{
    if (g_ServerBase != 0)
    {
        return TRUE; // Already initialized
    }

    HMODULE hServer = GetModuleHandleA("server.dll");
    if (!hServer)
    {
        logf("[HOOK] server.dll not found in process");
        return FALSE;
    }

    MODULEINFO mi = {0};
    if (GetModuleInformation(GetCurrentProcess(), hServer, &mi, sizeof(mi)))
    {
        g_ServerBase = (uintptr_t)mi.lpBaseOfDll;
        g_ServerSize = (size_t)mi.SizeOfImage;
        logf("[HOOK] server.dll mapped at %p, size: 0x%zx", (void *)g_ServerBase, g_ServerSize);
        return TRUE;
    }
    else
    {
        logf("[HOOK] Failed to get server.dll module info: %lu", GetLastError());
        return FALSE;
    }
}

/**
 * Determines if a calling function address is within the server.dll module range.
 * Used to selectively apply network fixes only to game's server code.
 *
 * Note: This method uses address range checking which is not 100% reliable.
 * A more robust solution would use stack walking but is significantly more complex.
 *
 * @param caller_addr The return address of the calling function
 * @return TRUE if caller is from server.dll, FALSE otherwise
 */
BOOL is_caller_from_server(uintptr_t caller_addr)
{
    if (g_ServerBase == 0)
    {
        init_server_module_range();
        if (g_ServerBase == 0)
        {
            logf("[HOOK] DEBUG: g_ServerBase is still 0 after init");
            return FALSE;
        }
    }

    BOOL inRange = (caller_addr >= g_ServerBase && caller_addr < g_ServerBase + g_ServerSize);
    return inRange;
}

/**
 * Hook for GetTickCount() Windows API function.
 * Provides fallback behavior in case the original function pointer is invalid.
 *
 * @return Tick count from original function or 0 as fallback
 */
DWORD WINAPI hook_GetTickCount(void)
{
    if (real_GetTickCount)
    {
        return real_GetTickCount();
    }
    else
    {
        logf("[SERVER HOOK] GetTickCount was NULL. Falling back to 0");
        return 0;
    }
}

/**
 * Hook for server.dll zlib_readFromStream function (RVA varies by version).
 * Fixes stability issues by preventing negative values in packet context.
 *
 * The original function can assign negative values to the error field which causes
 * network desynchronization and crashes.
 *
 * @param ctx Stream context structure pointer (validated for NULL)
 * @param received Number of bytes received
 * @param totalLen Total expected length
 * @return Modified return value (negative values converted to 0)
 */
int __cdecl hook_ServerFunction(int *ctx, int received, int totalLen)
{
    // Validate parameters
    if (!ctx)
    {
        logf("[SERVER HOOK] zlib_readFromStream called with NULL context");
        return -1;
    }

    // Call original function
    int ret = real_ServerFunction(ctx, received, totalLen);

    // Apply fixes to prevent network instability
    BOOL modified = false;
    if (ctx[0xE] < 0)
    {
        logf("[SERVER HOOK] zlib_readFromStream: Fixed negative ctx[0xE] (%d -> 0)", ctx[0xE]);
        ctx[0xE] = 0;
        modified = true;
    }

    if (ret < 0)
    {
        logf("[SERVER HOOK] zlib_readFromStream: Fixed negative return value (%d -> 0)", ret);
        ret = 0;
        modified = true;
    }

    if (modified)
    {
        logf("[SERVER HOOK] zlib_readFromStream: received=%d, totalLen=%d, result=%d", received, totalLen, ret);
    }

    return ret;
}

/**
 * Hook for recv() Winsock function to handle non-blocking socket errors.
 * Converts WSAEWOULDBLOCK errors to 0-byte receives for server.dll calls.
 *
 * The original game code doesn't handle WSAEWOULDBLOCK correctly, causing
 * desynchronization. This hook makes non-blocking sockets work gracefully.
 *
 * @param s Socket handle
 * @param buf Buffer to receive data into
 * @param len Buffer size
 * @param flags Recv flags (MSG_*)
 * @return Number of bytes received, 0 for graceful close, SOCKET_ERROR on error
 */
int WSAAPI hook_recv(SOCKET s, char *buf, int len, int flags)
{
    // Get caller address and check if it's from server.dll
    uintptr_t caller = (uintptr_t)CALLER_IP();
    if (!is_caller_from_server(caller))
    {
        return real_recv(s, buf, len, flags);
    }

    // Log socket buffer info on first use
    log_socket_buffer_info(s);

    // Validate parameters
    if (!buf || len <= 0)
    {
        logf("[WS2 HOOK] recv: Invalid parameters");
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }

    int result = real_recv(s, buf, len, flags);

    if (result == SOCKET_ERROR)
    {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
        {
            // Show buffer state when WSAEWOULDBLOCK occurs
            int available = get_available_bytes(s);
            if (available >= 0)
            {
                logf("[WS2 HOOK] recv: WSAEWOULDBLOCK, %d bytes available in buffer", available);
            }
            else
            {
                logf("[WS2 HOOK] recv: WSAEWOULDBLOCK, buffer state unknown");
            }

            // Convert WSAEWOULDBLOCK to 0 for server.dll calls
            WSASetLastError(NO_ERROR);
            return 0;
        }

        log_winsock_error("[WS2 HOOK] recv", s, error);
    }
    else if (result == 0)
    {
        logf("[WS2 HOOK] recv: Connection gracefully closed by peer on socket %u", (unsigned)s);
    }

    return result;
}

/**
 * Hook for send() Winsock function to add retry logic for partial sends.
 * Ensures all data is sent by retrying on WSAEWOULDBLOCK errors.
 *
 * The original game doesn't handle cases where send buffer is full,
 * leading to packet loss. This hook retries until all data is sent.
 *
 * @param s Socket handle
 * @param buf Data buffer to send
 * @param len Number of bytes to send
 * @param flags Send flags (MSG_*)
 * @return Total bytes sent, or SOCKET_ERROR on failure
 */
int WSAAPI hook_send(SOCKET s, const char *buf, int len, int flags)
{
    // Get caller address and check if it's from server.dll
    uintptr_t caller = (uintptr_t)CALLER_IP();
    if (!is_caller_from_server(caller))
    {
        return real_send(s, buf, len, flags);
    }

    // Log socket buffer info on first use
    log_socket_buffer_info(s);

    logf("[WS2 HOOK] send called from server.dll: socket=%u, len=%d, flags=0x%X", (unsigned)s, len, flags);

    // Validate parameters
    if (!buf || len <= 0)
    {
        logf("[WS2 HOOK] send: Invalid parameters");
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }

    int       total = 0;
    int       retry_count = 0;
    const int MAX_RETRIES = 1000; // Prevent infinite loops

    while (total < len && retry_count < MAX_RETRIES)
    {
        int sent = real_send(s, buf + total, len - total, flags);

        if (sent == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
            {
                logf("[WS2 HOOK] send: WSAEWOULDBLOCK, send buffer likely full (retry %d/%d)", retry_count + 1,
                     MAX_RETRIES);
                Sleep(1);
                retry_count++;
                continue;
            }

            log_winsock_error("[WS2 HOOK] send", s, error);
            WSASetLastError(error);
            if (error == WSAECONNRESET || error == WSAECONNABORTED)
            {
                return total > 0 ? total : SOCKET_ERROR;
            }
            return SOCKET_ERROR;
        }

        if (sent == 0)
        {
            logf("[WS2 HOOK] send: Connection closed by peer after %d/%d bytes", total, len);
            return total;
        }

        total += sent;
        retry_count = 0; // Reset retry counter on successful send
    }

    if (retry_count >= MAX_RETRIES)
    {
        logf("[WS2 HOOK] send: Max retries exceeded, sent %d/%d bytes (send buffer full)", total, len);
        WSASetLastError(WSAETIMEDOUT);
        return total > 0 ? total : SOCKET_ERROR;
    }

    return total;
}

/**
 * Reads server path configuration from game.ini file.
 * Looks for "Server" key in "[Network]" section.
 *
 * Uses GetPrivateProfileStringA() Windows API to parse INI file format.
 * Handles quoted paths and provides logging for troubleshooting.
 *
 * @param hModule Module handle to determine DLL location
 * @return Pointer to static buffer containing server path, or NULL on failure
 */
const char *get_server_path_from_ini(HMODULE hModule)
{
    static char serverPath[MAX_PATH];
    char        iniPath[MAX_PATH];

    if (hModule == NULL)
    {
        logf("[CONFIG] Module handle is NULL.");
        return NULL;
    }

    // Get the path of the DLL using GetModuleFileNameA()
    if (GetModuleFileNameA(hModule, iniPath, sizeof(iniPath)) == 0)
    {
        logf("[CONFIG] Failed to get module file name: %lu", GetLastError());
        return NULL;
    }

    // Find the last backslash and replace the filename with game.ini
    char *last_slash = strrchr(iniPath, '\\');
    if (last_slash)
    {
        strcpy(last_slash + 1, "game.ini");
    }
    else
    {
        logf("[CONFIG] Could not find backslash in module path: %s", iniPath);
        return NULL;
    }

    // Use GetPrivateProfileStringA() to read from INI file
    DWORD len = GetPrivateProfileStringA("Network", "Server",
                                         "", // Default value
                                         serverPath, sizeof(serverPath), iniPath);

    if (len > 0)
    {
        // Remove quotes if present
        if (serverPath[0] == '"' && serverPath[len - 1] == '"')
        {
            memmove(serverPath, serverPath + 1, len - 2);
            serverPath[len - 2] = '\0';
        }
        logf("[CONFIG] Read server path from game.ini: %s", serverPath);
        return serverPath;
    }

    logf("[CONFIG] Could not find 'Server' in '[Network]' section of %s", iniPath);
    return NULL;
}

/**
 * Creates and initializes all hook functions using MinHook library.
 * Sets up hooks for both Windows API functions and server.dll internals.
 *
 * @return TRUE if all hooks created successfully, FALSE if any failed
 */
static BOOL create_hooks(HMODULE hModule)
{
    MH_STATUS status;
    BOOL      success = true;

    // Get server path from game.ini or use default
    const char *serverPath = get_server_path_from_ini(hModule);
    if (!serverPath)
    {
        serverPath = "Server\\server.dll";
    }

    // Load server.dll and hook the target function using LoadLibraryA()
    HMODULE hServer = LoadLibraryA(serverPath);
    if (!hServer)
    {
        DWORD error = GetLastError();
        logf("[HOOK] Failed to load %s (error: %lu)", serverPath, error);
        success = false;
    }
    else
    {
        logf("[HOOK] Loaded %s at %p", serverPath, (void *)hServer);

        // Basic validation: check if module size is reasonable for server.dll
        MODULEINFO mi = {0};
        if (GetModuleInformation(GetCurrentProcess(), hServer, &mi, sizeof(mi)))
        {
            if (mi.SizeOfImage < 0x1000) // Minimum 4KB
            {
                logf("[HOOK] Warning: %s seems too small (0x%X bytes)", serverPath, mi.SizeOfImage);
            }
            logf("[HOOK] server.dll size: 0x%X bytes", mi.SizeOfImage);
        }

        // Use pre-detected server RVA from global variable
        DWORD TARGET_RVA = g_server_rva;
        if (TARGET_RVA == 0)
        {
            logf("[HOOK] No server RVA available - cannot create hook");
            success = false;
        }
        else if (TARGET_RVA >= mi.SizeOfImage)
        {
            logf("[HOOK] RVA 0x%X is beyond server.dll module size 0x%X", TARGET_RVA, mi.SizeOfImage);
            success = false;
        }
        else
        {
            void *targetAddr = (void *)((uintptr_t)hServer + TARGET_RVA);
            status = MH_CreateHook(targetAddr, hook_ServerFunction, (void **)&real_ServerFunction);
            if (status == MH_OK)
            {
                logf("[HOOK] Created hook for server function at %p (RVA +0x%X)", targetAddr, TARGET_RVA);
            }
            else
            {
                logf("[HOOK] Failed to create hook for server function: %d", (int)status);
                success = false;
            }
        }
    }

    // Hook Winsock functions to add retry logic and error handling using MH_CreateHookApi()
    status = MH_CreateHookApi(L"ws2_32", "recv", hook_recv, (void **)&real_recv);
    if (status == MH_OK)
    {
        logf("[HOOK] Created recv hook");
    }
    else
    {
        logf("[HOOK] Failed to create recv hook: %d", (int)status);
        success = false;
    }

    status = MH_CreateHookApi(L"ws2_32", "send", hook_send, (void **)&real_send);
    if (status == MH_OK)
    {
        logf("[HOOK] Created send hook");
    }
    else
    {
        logf("[HOOK] Failed to create send hook: %d", (int)status);
        success = false;
    }

    // Hook GetTickCount to prevent timer wraparound issues using MH_CreateHookApi()
    status = MH_CreateHookApi(L"kernel32", "GetTickCount", hook_GetTickCount, (void **)&real_GetTickCount);
    if (status == MH_OK)
    {
        logf("[HOOK] Created GetTickCount hook");
    }
    else
    {
        logf("[HOOK] Failed to create GetTickCount hook: %d", (int)status);
        success = false;
    }

    return success;
}

/**
 * Initializes the MinHook library and creates all hook functions.
 * Called from a separate thread to avoid DllMain deadlock issues.
 *
 * @return TRUE on success, FALSE on failure
 */
BOOL init_hooks(void)
{
    if (g_HooksInitialized)
    {
        return TRUE;
    }

    logf("[HOOK] Initialization started (PID: %lu, TID: %lu)", GetCurrentProcessId(), GetCurrentThreadId());

    // Early version detection - get server path and detect version before any hook operations
    extern HMODULE g_hModule; // Declared in main.c
    const char    *serverPath = get_server_path_from_ini(g_hModule);
    if (!serverPath)
    {
        serverPath = "Server\\server.dll";
    }

    logf("[HOOK] Detecting server version for: %s", serverPath);
    g_server_rva = detect_server_version(serverPath);
    if (g_server_rva == 0)
    {
        logf("[HOOK] Unknown server.dll version - cannot initialize hooks");
        return FALSE;
    }
    logf("[HOOK] Server version detected, will use RVA: 0x%X", g_server_rva);

    // Initialize server.dll module range
    init_server_module_range();

    // Initialize MinHook library using MH_Initialize()
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK)
    {
        logf("[HOOK] MH_Initialize failed: %d", (int)status);
        return FALSE;
    }

    logf("[HOOK] MinHook initialized successfully");

    // Create all hooks
    if (!g_hModule)
    {
        logf("[HOOK] Module handle is NULL, cannot create hooks");
        return FALSE;
    }

    if (!create_hooks(g_hModule))
    {
        logf("[HOOK] Some hooks failed to create");
        return FALSE;
    }

    // Enable all hooks using MH_EnableHook()
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status == MH_OK)
    {
        logf("[HOOK] All hooks enabled successfully");
        g_HooksInitialized = true;
    }
    else
    {
        logf("[HOOK] Failed to enable hooks: %d", (int)status);
        return FALSE;
    }

    // Smoke test - verify GetTickCount hook is working
    DWORD tickCount = GetTickCount();
    logf("[HOOK] GetTickCount test: %lu", tickCount);

    logf("[HOOK] Initialization completed successfully");
    return TRUE;
}

/**
 * Cleans up and disables all hooks when DLL is unloading.
 * Uses MH_DisableHook() and MH_Uninitialize() for cleanup.
 */
void cleanup_hooks(void)
{
    if (!g_HooksInitialized)
    {
        return;
    }

    logf("[HOOK] Cleanup started");

    MH_STATUS disableStatus = MH_DisableHook(MH_ALL_HOOKS);
    MH_STATUS uninitStatus = MH_Uninitialize();

    logf("[HOOK] Cleanup completed (Disable: %d, Uninit: %d)", (int)disableStatus, (int)uninitStatus);

    g_HooksInitialized = false;
}
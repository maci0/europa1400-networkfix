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
#include "pattern_matcher.h"
#include "sha256.h"
#include "versions.h"
#include <limits.h>
#include <psapi.h>
#include <shlwapi.h>
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

// Constants
#define DEFAULT_SERVER_PATH "Server\\server.dll"
#define SEND_MAX_RETRIES INT_MAX // Maximum retry attempts for send operations
#define SEND_RETRY_DELAY_MS 5    // Delay between send retries in milliseconds

// Global state
static BOOL      g_HooksInitialized = false;
static DWORD     g_server_rva = 0;
static HMODULE   g_hServerDll = NULL;
static uintptr_t g_server_base = 0;
static size_t    g_server_size = 0;

// Original function pointers
static int(WSAAPI *real_recv)(SOCKET, char *, int, int) = NULL;
static int(WSAAPI *real_send)(SOCKET, const char *, int, int) = NULL;
static DWORD(WINAPI *real_GetTickCount)(void) = NULL;

/* Server.dll srv_gameStreamReader function - RVA varies by version */
typedef int(__cdecl *srv_gameStreamReader_t)(int *ctx, int received, int totalLen);
static srv_gameStreamReader_t real_srv_gameStreamReader = NULL;

/**
 * Detect server.dll version by calculating its SHA256 hash.
 * Gets the module path and returns the RVA offset using pattern matching.
 *
 * @return RVA offset for the target function, or 0 if pattern matching fails
 */
static DWORD detect_server_version()
{
    if (!g_hServerDll)
    {
        logf("[HOOK] Invalid server module handle");
        return 0;
    }

    // Get the module file path directly as wide characters
    wchar_t serverPath[MAX_PATH];
    if (GetModuleFileNameW(g_hServerDll, serverPath, MAX_PATH) == 0)
    {
        logf("[HOOK] Failed to get module file name: %lu", GetLastError());
        return 0;
    }

    // Calculate file hash directly from wide path
    char fileHash[65]; // 64 chars + null terminator
    if (!calculate_file_sha256(serverPath, fileHash, sizeof(fileHash)))
    {
        logf("[HOOK] Failed to calculate SHA256 for server.dll");
        return 0;
    }

    logf("[HOOK] server.dll SHA256: %s", fileHash);

    // SHA256-based version lookup is currently disabled - using pattern matching instead
    DWORD checksum_rva = 0;
    /*for (int i = 0; known_versions[i].sha256_hash != NULL; i++)
    {
        if (strcmp(fileHash, known_versions[i].sha256_hash) == 0)
        {
            logf("[HOOK] Detected %s version (RVA: 0x%X)", known_versions[i].version_name,
                 known_versions[i].target_rva);
            checksum_rva = known_versions[i].target_rva;
            break;
        }
    }*/

    // Use pattern matching to find srv_gameStreamReader function
    DWORD                pattern_rva = 0;
    PATTERN_MATCH_RESULT result = find_srv_gameStreamReader_by_pattern(g_hServerDll, &pattern_rva);

    logf("[HOOK] Pattern matching result: %s", pattern_match_result_to_string(result));
    if (result == PATTERN_MATCH_SUCCESS)
    {
        logf("[HOOK] Pattern matcher found srv_gameStreamReader at RVA: 0x%X", pattern_rva);
        if (checksum_rva != 0)
        {
            if (pattern_rva == checksum_rva)
            {
                logf("[HOOK] Pattern matching SUCCESS: RVAs match (0x%X)", pattern_rva);
            }
            else
            {
                logf("[HOOK] Pattern matching MISMATCH: checksum=0x%X, pattern=0x%X", checksum_rva, pattern_rva);
            }
        }
    }
    else
    {
        logf("[HOOK] Pattern matching failed: %s", pattern_match_result_to_string(result));
    }

    if (pattern_rva == 0)
    {
        logf("[HOOK] Unknown server.dll version with hash: %s", fileHash);
    }

    logf("[HOOK] Server version detected, will use RVA: 0x%X", pattern_rva);

    return pattern_rva;
}

/**
 * Resets all global server-related variables to their initial state.
 * Used for cleanup on initialization failure.
 */
static void reset_server_globals(void)
{
    if (g_hServerDll)
    {
        FreeLibrary(g_hServerDll);
        g_hServerDll = NULL;
    }
    g_server_rva = 0;
    g_server_base = 0;
    g_server_size = 0;
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
 * Loads server.dll from the configured path.
 *
 * @param serverPath Path to server.dll to load
 * @return TRUE if loaded successfully, FALSE on error
 */
static BOOL load_server_dll(const char *serverPath)
{
    logf("[HOOK] Loading server.dll from: %s", serverPath);
    g_hServerDll = LoadLibraryA(serverPath);
    if (!g_hServerDll)
    {
        DWORD error = GetLastError();
        logf("[HOOK] Failed to load server.dll (error: %lu)", error);
        return FALSE;
    }
    Sleep(100); // avoid a race condition when loading the library while the game initialization hasn't quite finished
    logf("[HOOK] Server.dll loaded at %p", (void *)g_hServerDll);
    return TRUE;
}

/**
 * Initializes the server.dll module completely: loads library, detects version,
 * and sets up module range information.
 *
 * This function orchestrates all server.dll initialization logic.
 *
 * @return TRUE if initialization successful, FALSE on any error
 */
static BOOL init_server_module(void)
{
    if (g_hServerDll != NULL && g_server_rva != 0 && g_server_base != 0)
    {
        return TRUE; // Already fully initialized
    }

    // Get server path from game.ini or use default
    extern HMODULE g_hModule; // Declared in main.c
    const char    *serverPath = get_server_path_from_ini(g_hModule);
    if (!serverPath)
    {
        serverPath = DEFAULT_SERVER_PATH;
    }

    // Load, validate, and detect version
    if (!load_server_dll(serverPath) || (g_server_rva = detect_server_version()) == 0)
    {
        reset_server_globals();
        return FALSE;
    }

    // Get module information for range checking
    MODULEINFO module_info = {0};
    if (GetModuleInformation(GetCurrentProcess(), g_hServerDll, &module_info, sizeof(module_info)))
    {
        g_server_base = (uintptr_t)module_info.lpBaseOfDll;
        g_server_size = module_info.SizeOfImage;
        logf("[HOOK] Server module range: 0x%p - 0x%p (size: 0x%zX)", (void *)g_server_base,
             (void *)(g_server_base + g_server_size), g_server_size);
    }
    else
    {
        logf("[HOOK] Failed to get server module info: %lu", GetLastError());
        reset_server_globals();
        return FALSE;
    }

    return TRUE;
}

/**
 * Determines if a calling function address is within the server.dll module range.
 * Used to selectively apply network fixes only to game's server code.
 *
 * Uses simple range checking for performance - much faster than GetModuleHandleEx().
 *
 * @param s Socket handle (unused, kept for API compatibility)
 * @param caller_addr Address to check
 * @return TRUE if caller is from server.dll, FALSE otherwise
 */
BOOL is_caller_from_server(SOCKET s, uintptr_t caller_addr)
{
    // Check if server module is initialized
    if (g_server_base == 0 || g_server_size == 0)
    {
        return FALSE;
    }

    // Simple range check: is address within [base, base + size)?
    return (caller_addr >= g_server_base && caller_addr < g_server_base + g_server_size);
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
 * Hook for server.dll srv_gameStreamReader function (RVA varies by version).
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
int __cdecl hook_srv_gameStreamReader(int *ctx, int received, int totalLen)
{
    // Validate parameters
    if (!ctx)
    {
        logf("[SERVER HOOK] srv_gameStreamReader called with NULL context");
        return -1;
    }

    // Call original function
    int ret = real_srv_gameStreamReader(ctx, received, totalLen);

    // Apply fixes to prevent network instability
    BOOL modified = false;
    if (ctx[0xE] < 0)
    {
        logf("[SERVER HOOK] srv_gameStreamReader: Fixed negative ctx[0xE] (%d -> 0)", ctx[0xE]);
        ctx[0xE] = 0;
        modified = true;
    }

    if (ret < 0)
    {
        logf("[SERVER HOOK] srv_gameStreamReader: Fixed negative return value (%d -> 0)", ret);
        ret = 0;
        modified = true;
    }

    if (modified)
    {
        logf("[SERVER HOOK] srv_gameStreamReader: received=%d, totalLen=%d, result=%d", received, totalLen, ret);
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
    // Check if caller is from server.dll and set up logging

    if (!is_caller_from_server(s, (uintptr_t)CALLER_IP()))
    {
        return real_recv(s, buf, len, flags);
    }

    // Log suspicious parameters but don't block - let Windows handle them
    // (Original HarryTheBird version passed all params through directly)
    if (!buf || len <= 0)
    {
        logf("[WS2 HOOK] recv: Suspicious parameters: buf=%p, len=%d (hex=0x%08X)", buf, len, (unsigned int)len);
    }

    int result = real_recv(s, buf, len, flags);

    if (result == SOCKET_ERROR)
    {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
        {
            // Show buffer state when WSAEWOULDBLOCK occurs (rate limited)
            int available = get_available_bytes(s);
            if (available >= 0)
            {
                logf_rate_limited("recv_wouldblock", "[WS2 HOOK] recv: WSAEWOULDBLOCK, %d bytes available in buffer",
                                  available);
            }
            else
            {
                logf_rate_limited("recv_wouldblock_unknown", "[WS2 HOOK] recv: WSAEWOULDBLOCK, buffer state unknown");
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
        log_socket_buffer_info(s);
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
    // Check if caller is from server.dll and set up logging
    if (!is_caller_from_server(s, (uintptr_t)CALLER_IP()))
    {
        return real_send(s, buf, len, flags);
    }

    logf_rate_limited("send_called", "[WS2 HOOK] send: called from server.dll: socket=%u, len=%d, flags=0x%X",
                      (unsigned)s, len, flags);

    // Log suspicious parameters but don't block - let the loop handle them naturally
    // (Original HarryTheBird version: while(total < len) exits immediately if len <= 0)
    if (!buf || len <= 0)
    {
        logf("[WS2 HOOK] send: Suspicious parameters: buf=%p, len=%d (hex=0x%08X)", buf, len, (unsigned int)len);
    }

    int total = 0;
    int retry_count = 0;

    while (total < len && retry_count < SEND_MAX_RETRIES)
    {
        int sent = real_send(s, buf + total, len - total, flags);

        if (sent == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
            {
                logf_rate_limited("send_wouldblock",
                                  "[WS2 HOOK] send: WSAEWOULDBLOCK, send buffer likely full (retry %d/%d)",
                                  retry_count + 1, SEND_MAX_RETRIES);
                Sleep(SEND_RETRY_DELAY_MS);
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

    if (retry_count >= SEND_MAX_RETRIES)
    {
        logf_rate_limited("send_max_retries",
                          "[WS2 HOOK] send: Max retries exceeded, sent %d/%d bytes (send buffer full)", total, len);
        log_socket_buffer_info(s);
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

    // Remove filename and append game.ini using Path API
    if (!PathRemoveFileSpecA(iniPath))
    {
        logf("[CONFIG] Could not remove file spec from module path: %s", iniPath);
        return NULL;
    }

    if (!PathCombineA(iniPath, iniPath, "game.ini"))
    {
        logf("[CONFIG] Could not combine path with game.ini");
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
 * Helper function to create API hooks with consistent logging.
 * Reduces code duplication in hook creation.
 *
 * @param module Module name (L"ws2_32", L"kernel32", etc.)
 * @param function Function name to hook
 * @param hook_func Hook function pointer
 * @param original_func Pointer to store original function pointer
 * @param hook_name Name for logging (e.g., "recv", "send")
 * @return TRUE if hook created successfully, FALSE otherwise
 */
static BOOL create_hook_api(const wchar_t *module, const char *function, void *hook_func, void **original_func,
                            const char *hook_name)
{
    MH_STATUS status = MH_CreateHookApi(module, function, hook_func, original_func);
    if (status == MH_OK)
    {
        logf("[HOOK] Created %s hook", hook_name);
        return TRUE;
    }
    else
    {
        logf("[HOOK] Failed to create %s hook: %d", hook_name, (int)status);
        return FALSE;
    }
}

/**
 * Creates and initializes all hook functions using MinHook library.
 * Sets up hooks for both Windows API functions and server.dll internals.
 *
 * @return TRUE if all hooks created successfully, FALSE if any failed
 */
static BOOL create_hooks(void)
{
    BOOL success = TRUE;

    // Create hook for server.dll function using pre-initialized values
    if (!g_hServerDll || g_server_rva == 0)
    {
        logf("[HOOK] Server module not properly initialized - cannot create server hook");
        success = FALSE;
    }
    else
    {
        void     *targetAddr = (void *)((uintptr_t)g_hServerDll + g_server_rva);
        MH_STATUS status = MH_CreateHook(targetAddr, hook_srv_gameStreamReader, (void **)&real_srv_gameStreamReader);
        if (status == MH_OK)
        {
            logf("[HOOK] Created hook for server function at %p (RVA +0x%X)", targetAddr, g_server_rva);
        }
        else
        {
            logf("[HOOK] Failed to create hook for server function: %d", (int)status);
            success = FALSE;
        }
    }

    // Create API hooks using helper function
    success &= create_hook_api(L"ws2_32", "recv", hook_recv, (void **)&real_recv, "recv");
    success &= create_hook_api(L"ws2_32", "send", hook_send, (void **)&real_send, "send");
    success &=
        create_hook_api(L"kernel32", "GetTickCount", hook_GetTickCount, (void **)&real_GetTickCount, "GetTickCount");

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

    // Initialize server.dll module (load, detect version, set up ranges)
    if (!init_server_module())
    {
        logf("[HOOK] Failed to initialize server module");
        return FALSE;
    }

    // Initialize MinHook library using MH_Initialize()
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK)
    {
        logf("[HOOK] MH_Initialize failed: %d", (int)status);
        return FALSE;
    }

    logf("[HOOK] MinHook initialized successfully");

    // Create all hooks
    if (!create_hooks())
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

    // Free the globally loaded server.dll

    if (g_hServerDll)
    {
        logf("[HOOK] Freeing server.dll handle");
        FreeLibrary(g_hServerDll);
        g_hServerDll = NULL;
    }

    g_HooksInitialized = false;
}
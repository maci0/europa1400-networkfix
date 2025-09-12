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

// Constants
#define DEFAULT_SERVER_PATH "Server\\server.dll"
#define SEND_MAX_RETRIES 1000 // Maximum retry attempts for send operations
#define SEND_RETRY_DELAY_MS 1 // Delay between send retries in milliseconds

// Global state
static BOOL      g_HooksInitialized = false;
static uintptr_t g_ServerBase = 0;
static size_t    g_ServerSize = 0;
static DWORD     g_server_rva = 0;
static HMODULE   g_hServerDll = NULL;

// Original function pointers
static int(WSAAPI *real_recv)(SOCKET, char *, int, int) = NULL;
static int(WSAAPI *real_send)(SOCKET, const char *, int, int) = NULL;
static DWORD(WINAPI *real_GetTickCount)(void) = NULL;

/* Server.dll srv_gameStreamReader function - RVA varies by version */
typedef int(__cdecl *srv_gameStreamReader_t)(int *ctx, int received, int totalLen);
static srv_gameStreamReader_t real_srv_gameStreamReader = NULL;

/**
 * Detect server.dll version by calculating its SHA256 hash.
 * Infers the path from the module handle and returns the RVA offset.
 *
 * @return RVA offset for the target function, or 0 if unknown version
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

    // Look up version in known list
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

    // Test pattern matching functionality (for testing only - not used yet)
    DWORD                pattern_rva = 0;
    PATTERN_MATCH_RESULT result = find_srv_gameStreamReader_by_pattern(g_hServerDll, &pattern_rva);

    logf("[HOOK] Pattern matching test: %s", pattern_match_result_to_string(result));
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
    g_ServerBase = 0;
    g_ServerSize = 0;
    g_server_rva = 0;
}

/**
 * Gets module information for a loaded module.
 * Common pattern helper to reduce code duplication.
 *
 * @param hModule Module handle
 * @param mi Pointer to MODULEINFO structure to fill
 * @return TRUE if successful, FALSE otherwise
 */
static BOOL get_module_info(HMODULE hModule, MODULEINFO *mi)
{
    if (!GetModuleInformation(GetCurrentProcess(), hModule, mi, sizeof(*mi)))
    {
        logf("[HOOK] Failed to get module info: %lu", GetLastError());
        return FALSE;
    }
    return TRUE;
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
 * Validates Winsock function parameters.
 * Common validation for recv/send operations.
 *
 * @param buf Buffer pointer to validate
 * @param len Buffer length to validate
 * @return TRUE if parameters are valid, FALSE otherwise (sets WSA error)
 */
static BOOL validate_winsock_params(const void *buf, int len)
{
    if (!buf || len <= 0)
    {
        logf("[WS2 HOOK] Invalid parameters: buf=%p, len=%d", buf, len);
        WSASetLastError(WSAEINVAL);
        return FALSE;
    }
    return TRUE;
}

/**
 * Checks if caller is from server.dll and sets up logging.
 * Common pattern for both recv/send hooks.
 *
 * @param s Socket handle for logging setup
 * @return TRUE if caller is from server.dll, FALSE otherwise
 */
static BOOL check_server_caller(SOCKET s)
{
    uintptr_t caller = (uintptr_t)CALLER_IP();
    if (!is_caller_from_server(caller))
    {
        return FALSE;
    }

    // Log socket buffer info on first use from server.dll
    log_socket_buffer_info(s);
    return TRUE;
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
 * Validates and sets up server module information.
 * Gets base address, size, and performs basic validation.
 *
 * @return TRUE if validation successful, FALSE on error
 */
static BOOL validate_server_module(void)
{
    MODULEINFO mi = {0};
    if (!get_module_info(g_hServerDll, &mi))
    {
        reset_server_globals();
        return FALSE;
    }

    g_ServerBase = (uintptr_t)mi.lpBaseOfDll;
    g_ServerSize = (size_t)mi.SizeOfImage;
    logf("[HOOK] server.dll mapped at %p, size: 0x%zx", (void *)g_ServerBase, g_ServerSize);

    // Basic validation: check if module size is reasonable for server.dll
    if (mi.SizeOfImage < 0x1000) // Minimum 4KB
    {
        logf("[HOOK] Warning: server.dll seems too small (0x%X bytes)", mi.SizeOfImage);
    }

    return TRUE;
}

/**
 * Detects server version and sets up RVA information.
 * Uses the global server handle to infer version information.
 *
 * @return TRUE if version detected successfully, FALSE on error
 */
static BOOL detect_server_version_info(void)
{
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
    if (g_hServerDll != NULL && g_ServerBase != 0 && g_server_rva != 0)
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
    if (!load_server_dll(serverPath) || !validate_server_module() || !detect_server_version())
    {
        reset_server_globals();
        return FALSE;
    }

    return TRUE;
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
        logf("[HOOK] DEBUG: g_ServerBase is 0 - server module not initialized");
        return FALSE;
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
    if (!check_server_caller(s))
    {
        return real_recv(s, buf, len, flags);
    }

    // Validate parameters
    if (!validate_winsock_params(buf, len))
    {
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
    // Check if caller is from server.dll and set up logging
    if (!check_server_caller(s))
    {
        return real_send(s, buf, len, flags);
    }

    logf("[WS2 HOOK] send called from server.dll: socket=%u, len=%d, flags=0x%X", (unsigned)s, len, flags);

    // Validate parameters
    if (!validate_winsock_params(buf, len))
    {
        return SOCKET_ERROR;
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
                logf("[WS2 HOOK] send: WSAEWOULDBLOCK, send buffer likely full (retry %d/%d)", retry_count + 1,
                     SEND_MAX_RETRIES);
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
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
// Cap retries to avoid hanging the game thread forever on a persistently
// full send buffer (peer not reading). 5000 * 1ms ≈ 5 s before we give
// up with WSAETIMEDOUT — long enough for normal VPN jitter, finite
// enough to avoid an INT_MAX busy-loop that overflows signed int.
#define SEND_MAX_RETRIES 5000
#define SEND_RETRY_DELAY_MS 1 // Delay between send retries (matches original)

#ifdef NETWORKFIX_TEST
// Test build: real_recv/real_send are externally writable mocks.
// Sleep is redirected to a counter so retry loops do not waste wallclock time.
#define HOOK_STATIC
void test_sleep(DWORD ms);
#define HOOK_SLEEP(ms) test_sleep(ms)
#else
#define HOOK_STATIC static
#define HOOK_SLEEP(ms) Sleep(ms)
#endif

// Global state
static BOOL g_HooksInitialized = false;
// When false (NETWORKFIX_DISABLE=1) the ws2/stream hooks are still installed
// (so harness fault-injection, tracing and fastsync keep working) but they
// pass through with the game's original semantics: this is the A/B baseline.
static BOOL      g_fix_active = true;
static DWORD     g_server_rva = 0;
static HMODULE   g_hServerDll = NULL;
static uintptr_t g_server_base = 0;
static size_t    g_server_size = 0;

// Original function pointers
HOOK_STATIC int(WSAAPI *real_recv)(SOCKET, char *, int, int) = NULL;
HOOK_STATIC int(WSAAPI *real_send)(SOCKET, const char *, int, int) = NULL;
static DWORD(WINAPI *real_GetTickCount)(void) = NULL;

/* Server.dll srv_gameStreamReader function - RVA varies by version */
typedef int(__cdecl *srv_gameStreamReader_t)(int *ctx, int received, int totalLen);
HOOK_STATIC srv_gameStreamReader_t real_srv_gameStreamReader = NULL;

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
    DWORD   pathLen = GetModuleFileNameW(g_hServerDll, serverPath, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH)
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

    // Try pattern matching first
    DWORD                pattern_rva = 0;
    PATTERN_MATCH_RESULT result = find_srv_gameStreamReader_by_pattern(g_hServerDll, &pattern_rva);

    if (result == PATTERN_MATCH_SUCCESS)
    {
        logf("[HOOK] Pattern matcher found srv_gameStreamReader at RVA: 0x%X", pattern_rva);
        return pattern_rva;
    }

    logf("[HOOK] Pattern matching failed: %s", pattern_match_result_to_string(result));

    // Fallback to SHA256-based version lookup
    for (int i = 0; known_versions[i].sha256_hash != NULL; i++)
    {
        if (strcmp(fileHash, known_versions[i].sha256_hash) == 0)
        {
            logf("[HOOK] Fallback: Detected %s version (RVA: 0x%X)", known_versions[i].version_name,
                 known_versions[i].target_rva);
            return known_versions[i].target_rva;
        }
    }

    logf("[HOOK] Unknown server.dll version with hash: %s", fileHash);
    return 0;
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
#ifdef NETWORKFIX_TEST
#define PATH_STATIC
#else
#define PATH_STATIC static
#endif

PATH_STATIC BOOL is_safe_server_path(const char *path)
{
    if (!path || !path[0])
        return FALSE;
    // Reject absolute paths (drive letter, UNC, leading slash)
    if (path[1] == ':' || path[0] == '\\' || path[0] == '/')
        return FALSE;
    // Reject traversal
    if (strstr(path, ".."))
        return FALSE;
    // Restrict to .dll files
    size_t len = strlen(path);
    if (len < 4 || _stricmp(path + len - 4, ".dll") != 0)
        return FALSE;
    return TRUE;
}

/* True iff `path` is `dir` itself or a descendant. Requires a directory
 * separator after `dir` so "C:\Guild" does not match "C:\GuildExtra\...". */
PATH_STATIC BOOL path_is_within_dir(const char *path, const char *dir)
{
    if (!path || !dir || !path[0] || !dir[0])
        return FALSE;
    size_t dlen = strlen(dir);
    if (_strnicmp(path, dir, dlen) != 0)
        return FALSE;
    if (path[dlen] == '\0')
        return TRUE;
    return path[dlen] == '\\' || path[dlen] == '/';
}

static BOOL load_server_dll(const char *serverPath)
{
    logf("[HOOK] Loading server.dll from: %s", serverPath);
    if (!is_safe_server_path(serverPath))
    {
        logf("[HOOK] Rejected unsafe server path: %s", serverPath);
        return FALSE;
    }
    // Verify canonical path stays within game dir
    char  moduleDir[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(g_hModule, moduleDir, sizeof(moduleDir));
    if (n != 0 && n < sizeof(moduleDir))
    {
        PathRemoveFileSpecA(moduleDir);
        char combined[MAX_PATH] = {0}, full[MAX_PATH] = {0}, canonical[MAX_PATH] = {0};
        if (PathCombineA(combined, moduleDir, serverPath) && GetFullPathNameA(combined, sizeof(full), full, NULL) &&
            GetFullPathNameA(full, sizeof(canonical), canonical, NULL))
        {
            // Must be under game dir (prefix match alone accepts C:\GuildExtra)
            char gameDirCanonical[MAX_PATH] = {0};
            GetFullPathNameA(moduleDir, sizeof(gameDirCanonical), gameDirCanonical, NULL);
            if (!path_is_within_dir(canonical, gameDirCanonical))
            {
                logf("[HOOK] Rejected path outside game dir: %s -> %s", serverPath, canonical);
                return FALSE;
            }
            logf("[HOOK] Canonical server path: %s", canonical);
            g_hServerDll = LoadLibraryA(canonical);
            if (!g_hServerDll)
            {
                logf("[HOOK] Failed to load server.dll (error: %lu)", GetLastError());
                return FALSE;
            }
            logf("[HOOK] Server.dll loaded at %p", (void *)g_hServerDll);
            return TRUE;
        }
    }
    g_hServerDll = LoadLibraryA(serverPath);
    if (!g_hServerDll)
    {
        DWORD error = GetLastError();
        logf("[HOOK] Failed to load server.dll (error: %lu)", error);
        return FALSE;
    }
    // No Sleep(100) — LoadLibrary is synchronous; original race comment not reproducible.
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
    const char *serverPath = get_server_path_from_ini(g_hModule);
    if (!serverPath)
    {
        serverPath = DEFAULT_SERVER_PATH;
    }

    // TOCTOU mitigation: hash the file before LoadLibrary when we have a filesystem path,
    // then verify after load that the mapped image matches (pattern match is primary).
    // If file can't be hashed pre-load, fall back to post-load detection only.
    char preHash[65] = {0};
    BOOL hasPreHash = FALSE;
    {
        // Build absolute file path for pre-hash (best-effort)
        char  moduleDir[MAX_PATH] = {0};
        char  filePath[MAX_PATH] = {0};
        DWORD pathLen = GetModuleFileNameA(g_hModule, moduleDir, sizeof(moduleDir));
        if (pathLen != 0 && pathLen < sizeof(moduleDir) && PathRemoveFileSpecA(moduleDir) &&
            PathCombineA(filePath, moduleDir, serverPath))
        {
            wchar_t wpath[MAX_PATH];
            if (MultiByteToWideChar(CP_ACP, 0, filePath, -1, wpath, MAX_PATH) != 0)
            {
                if (calculate_file_sha256(wpath, preHash, sizeof(preHash)))
                {
                    hasPreHash = TRUE;
                    logf("[HOOK] Pre-load SHA256: %s", preHash);
                    // Optional allowlist: if hash known, we know it's whitelisted; if not,
                    // we still allow load but pattern matcher must succeed post-load.
                }
            }
        }
    }

    // Load, validate, and detect version (pattern matcher does post-load validation)
    if (!load_server_dll(serverPath) || (g_server_rva = detect_server_version()) == 0)
    {
        reset_server_globals();
        return FALSE;
    }

    // Verify pre-hash matches post-load hash if we had one (detect TOCTOU replacement)
    if (hasPreHash)
    {
        wchar_t loadedPath[MAX_PATH];
        DWORD   loadedLen = GetModuleFileNameW(g_hServerDll, loadedPath, MAX_PATH);
        if (loadedLen != 0 && loadedLen < MAX_PATH)
        {
            char postHash[65] = {0};
            if (calculate_file_sha256(loadedPath, postHash, sizeof(postHash)))
            {
                if (strcmp(preHash, postHash) != 0)
                {
                    logf("[HOOK] Hash mismatch pre/post load: %s != %s — possible replacement, aborting", preHash,
                         postHash);
                    reset_server_globals();
                    return FALSE;
                }
            }
        }
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
 * @param caller_addr Address to check
 * @return TRUE if caller is from server.dll, FALSE otherwise
 */
#ifdef NETWORKFIX_TEST
BOOL g_test_force_caller_server = TRUE; // test hook to exercise non-server path
#endif

BOOL is_caller_from_server(uintptr_t caller_addr)
{
#ifdef NETWORKFIX_TEST
    if (!g_test_force_caller_server)
        return FALSE;
    (void)caller_addr;
    return TRUE;
#else
    // Check if server module is initialized
    if (g_server_base == 0 || g_server_size == 0)
    {
        return FALSE;
    }

    // Simple range check: is address within [base, base + size)?
    return (caller_addr >= g_server_base && caller_addr < g_server_base + g_server_size);
#endif
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
    // Baseline (fix off): pass straight through with no NULL guard or clamping,
    // so the unpatched desync behaviour is faithfully reproduced for A/B.
    if (!g_fix_active)
    {
        return real_srv_gameStreamReader(ctx, received, totalLen);
    }

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

/*
 * TCP_NODELAY: the game never sets it (verified: server.dll has no
 * IPPROTO_TCP setsockopt), so Nagle's algorithm holds every sub-MSS packet
 * until the previous one is ACKed. The protocol is small-message and latency
 * sensitive (command packets, per-tick sync), so on any link with a non-trivial
 * delayed-ACK Nagle adds up to a full delayed-ACK interval (~40ms on Linux) of
 * latency per exchange. Disabling it is safe for this traffic shape and only
 * helps. Applied once per server.dll socket; part of the fix, so the A/B
 * baseline (g_fix_active=false) keeps the original Nagle behaviour. Force
 * Nagle back on for testing with NETWORKFIX_NODELAY=0. */
static void maybe_set_nodelay(SOCKET s)
{
    if (!g_fix_active)
        return;

    static int    enabled = -1; // -1 unknown, 0 off, 1 on
    static SOCKET seen[64];
    static int    seen_n = 0;
    if (enabled == -1)
    {
        char v[2] = {0};
        // Default on; only "0" disables.
        enabled = (GetEnvironmentVariableA("NETWORKFIX_NODELAY", v, sizeof(v)) == 1 && v[0] == '0') ? 0 : 1;
    }
    if (enabled == 0)
        return;
    for (int i = 0; i < seen_n; i++)
        if (seen[i] == s)
            return;
    if (seen_n < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[seen_n++] = s;
    int one = 1;
    if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one)) == 0)
        logf("[NODELAY] socket=%u TCP_NODELAY enabled (Nagle off)", (unsigned)s);
    else
        logf_rate_limited("nodelay_fail", "[NODELAY] socket=%u setsockopt failed: %d", (unsigned)s, WSAGetLastError());
}

/* Harness-only fault injection (HARNESS_TINY_BUFFERS=N): shrink SO_SNDBUF/
 * SO_RCVBUF on each server.dll socket to N bytes the first time we see it, so
 * send() hits WSAEWOULDBLOCK constantly during active play. This reproduces
 * the exact desync the fix targets (the original game does not retry a partial
 * send), without needing host kernel netem. Applied once per socket. */
static void maybe_shrink_buffers(SOCKET s)
{
    static int    tiny = -1; // -1 unknown, 0 off, >0 target bytes
    static SOCKET seen[64];
    static int    seen_n = 0;
    if (tiny == -1)
    {
        char v[8] = {0};
        tiny = (GetEnvironmentVariableA("HARNESS_TINY_BUFFERS", v, sizeof(v)) > 0) ? atoi(v) : 0;
        if (tiny < 0)
            tiny = 0;
    }
    if (tiny == 0)
        return;
    for (int i = 0; i < seen_n; i++)
        if (seen[i] == s)
            return;
    if (seen_n < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[seen_n++] = s;
    int val = tiny;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&val, sizeof(val));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&val, sizeof(val));
    logf("[TINY BUF] socket=%u SO_SNDBUF/SO_RCVBUF set to %d bytes", (unsigned)s, val);
}

/* Harness-only payload tracing (HARNESS_NET_TRACE=1): hex-dump the first bytes
 * of server.dll traffic so protocol stalls can be diagnosed from hook_log. */
static void trace_payload(const char *dir, const char *buf, int len)
{
    static int trace_state = -1; // -1 unknown, 0 off, 1 on
    if (trace_state == -1)
    {
        char v[2] = {0};
        trace_state = (GetEnvironmentVariableA("HARNESS_NET_TRACE", v, sizeof(v)) == 1 && v[0] == '1') ? 1 : 0;
    }
    if (trace_state != 1 || !buf || len <= 0)
        return;
    char hex[3 * 48 + 1];
    int  n = len < 48 ? len : 48;
    for (int i = 0; i < n; i++)
        sprintf(hex + i * 3, "%02X ", (unsigned char)buf[i]);
    hex[n * 3] = '\0';
    logf("[NET TRACE] %s len=%d: %s", dir, len, hex);
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
    // Check if caller is from server.dll
    if (!is_caller_from_server((uintptr_t)CALLER_IP()))
    {
        return real_recv(s, buf, len, flags);
    }

    // Log suspicious parameters but don't block - let Windows handle them
    // (Original HarryTheBird version passed all params through directly)
    if (!buf || len <= 0)
    {
        logf("[WS2 HOOK] recv: Suspicious parameters: buf=%p, len=%d (hex=0x%08X)", buf, len, (unsigned int)len);
    }

    maybe_set_nodelay(s);
    maybe_shrink_buffers(s);
    int result = real_recv(s, buf, len, flags);
    if (result > 0)
    {
        trace_payload("recv", buf, result);
    }

    // Baseline (fix off): return exactly what Windows returned, no WSAEWOULDBLOCK
    // conversion, matching the unpatched game.
    if (!g_fix_active)
    {
        return result;
    }

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
    // Check if caller is from server.dll
    if (!is_caller_from_server((uintptr_t)CALLER_IP()))
    {
        return real_send(s, buf, len, flags);
    }

    logf_rate_limited("send_called", "[WS2 HOOK] send: called from server.dll: socket=%u, len=%d, flags=0x%X",
                      (unsigned)s, len, flags);
    maybe_set_nodelay(s);
    maybe_shrink_buffers(s);
    trace_payload("send", buf, len);

    // Baseline (fix off): reproduce the original game's single, no-retry send.
    if (!g_fix_active)
    {
        return real_send(s, buf, len, flags);
    }

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
                HOOK_SLEEP(SEND_RETRY_DELAY_MS);
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
    DWORD iniLen = GetModuleFileNameA(hModule, iniPath, sizeof(iniPath));
    if (iniLen == 0 || iniLen >= sizeof(iniPath))
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

    // PathCombineA does not support overlapping buffers (pszDest == pszDir).
    // Use a temporary buffer to avoid undefined behavior (verified against
    // MSDN: pszDest should not overlap pszDir/pszFile).
    {
        char combined[MAX_PATH];
        if (!PathCombineA(combined, iniPath, "game.ini"))
        {
            logf("[CONFIG] Could not combine path with game.ini");
            return NULL;
        }
        // Safe copy back to iniPath (combined was built from iniPath, no overflow)
        strcpy(iniPath, combined);
    }

    // Use GetPrivateProfileStringA() to read from INI file
    // Support both ServerPath (documented) and Server (legacy) keys for backwards compat.
    DWORD len = GetPrivateProfileStringA("Network", "ServerPath",
                                         "", // Default value
                                         serverPath, sizeof(serverPath), iniPath);
    if (len == 0)
    {
        len = GetPrivateProfileStringA("Network", "Server",
                                       "", // Fallback legacy key
                                       serverPath, sizeof(serverPath), iniPath);
    }

    if (len > 0)
    {
        // Remove surrounding quotes if present (handles paths with spaces)
        if (len >= 2 && serverPath[0] == '"' && serverPath[len - 1] == '"')
        {
            memmove(serverPath, serverPath + 1, len - 2);
            serverPath[len - 2] = '\0';
        }
        logf("[CONFIG] Read server path from game.ini: %s", serverPath);
        return serverPath;
    }

    logf("[CONFIG] Could not find 'ServerPath'/'Server' in '[Network]' section of %s", iniPath);
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
    if (!module || !function || !hook_func || !original_func)
    {
        logf("[HOOK] Invalid params for %s hook", hook_name ? hook_name : "(null)");
        return FALSE;
    }
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

/*
 * Harness-only guard for the game's evt poll loop (Europa1400Gold_TL.exe
 * 0x429800). The loop dereferences the evt:command_entry table pointer at
 * [0x6B7E94] every tick; under headless Wine the tick fires while the table
 * is not (yet/anymore) allocated, faulting at 0x42980D. The allocator
 * (0x429920) publishes 0x6B7E94 last and the free path (0x4298C8) clears it,
 * so a NULL check on that single pointer is sufficient to skip the poll.
 * Opt-in via HARNESS_EVT_GUARD=1; the target bytes are signature-checked so
 * any other game build is left untouched.
 */
#define EVT_POLL_ADDR ((LPVOID)0x429800)
#define EVT_TABLE_PTR ((void *volatile *)0x6B7E94)
/* push ebx; push esi; xor ebx,ebx; xor esi,esi; mov eax,[0x6B7E94]; add eax,esi */
static const uint8_t evt_poll_sig[] = {0x53, 0x56, 0x33, 0xDB, 0x33, 0xF6, 0xA1, 0x94, 0x7E, 0x6B, 0x00, 0x03, 0xC6};
static void(__cdecl *real_evt_poll)(void) = NULL;

static void __cdecl hook_evt_poll(void)
{
    if (*EVT_TABLE_PTR == NULL)
    {
        logf_rate_limited("evt_guard", "[EVT GUARD] evt table NULL, poll skipped");
        return;
    }
    real_evt_poll();
}

/* Reads a "1"-valued environment flag (harness contract). */
static BOOL env_flag(const char *name)
{
    char value[2] = {0};
    return GetEnvironmentVariableA(name, value, sizeof(value)) == 1 && value[0] == '1';
}

/*
 * Fast sync: server.dll's network pump thread is throttled by a hardcoded
 * Sleep(30) and dequeues exactly ONE queued message per connection per tick
 * (pump loop 0x95a1..0x95c6 in the GOG build, send-pump 0xa860). The whole
 * town snapshot (~5300 x 145-byte messages) is queued up front, so a
 * multiplayer game load takes ~160 s at ~10 KB/s while the TCP link and both
 * consumers sit idle (verified by strace: the only wait between sends is the
 * Sleep itself). Clamping that Sleep(30) to 1 ms makes the transfer complete
 * in seconds without touching any other timing (the game exe's timers are
 * unaffected: the patch is on server.dll's own IAT). Disable with
 * NETWORKFIX_FASTSYNC=0.
 */
static VOID(WINAPI *real_server_sleep)(DWORD) = NULL;
static IMAGE_THUNK_DATA *g_server_sleep_iat = NULL;

static VOID WINAPI       hook_server_sleep(DWORD ms)
{
    if (ms == 30)
    {
        logf_rate_limited("fastsync", "[FASTSYNC] server.dll pump Sleep(30) clamped to 1 ms");
        ms = 1; // keep yielding so the pump thread never busy-spins
    }
    real_server_sleep(ms);
}

static void patch_server_sleep_iat(void)
{
    {
        char v[2] = {0};
        if (GetEnvironmentVariableA("NETWORKFIX_FASTSYNC", v, sizeof(v)) == 1 && v[0] == '0')
        {
            logf("[FASTSYNC] Disabled via NETWORKFIX_FASTSYNC=0");
            return;
        }
    }

    if (!g_hServerDll || g_server_size == 0)
        return;

    const BYTE             *base = (const BYTE *)g_hServerDll;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (g_server_size < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > g_server_size)
        return;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;
    const IMAGE_DATA_DIRECTORY *dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir->VirtualAddress == 0 || dir->Size == 0)
        return;
    if ((size_t)dir->VirtualAddress + sizeof(IMAGE_IMPORT_DESCRIPTOR) > g_server_size)
        return;

    const IMAGE_IMPORT_DESCRIPTOR *imp = (const IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
    const IMAGE_IMPORT_DESCRIPTOR *imp_end = (const IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress + dir->Size);
    for (; imp < imp_end && imp->Name != 0; imp++)
    {
        if ((size_t)imp->Name >= g_server_size)
            continue;
        const char *dllName = (const char *)(base + imp->Name);
        if (_stricmp(dllName, "KERNEL32.dll") != 0)
            continue;
        if (imp->OriginalFirstThunk == 0 || imp->FirstThunk == 0)
            continue;
        if ((size_t)imp->OriginalFirstThunk + sizeof(IMAGE_THUNK_DATA) > g_server_size ||
            (size_t)imp->FirstThunk + sizeof(IMAGE_THUNK_DATA) > g_server_size)
            continue;
        const IMAGE_THUNK_DATA *nameThunk = (const IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA       *iatThunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; nameThunk->u1.AddressOfData != 0; nameThunk++, iatThunk++)
        {
            if (nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            if ((size_t)nameThunk->u1.AddressOfData + sizeof(IMAGE_IMPORT_BY_NAME) > g_server_size)
                continue;
            const IMAGE_IMPORT_BY_NAME *ibn = (const IMAGE_IMPORT_BY_NAME *)(base + nameThunk->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, "Sleep") != 0)
                continue;
            DWORD oldProtect;
            if (!VirtualProtect(&iatThunk->u1.Function, sizeof(iatThunk->u1.Function), PAGE_READWRITE, &oldProtect))
            {
                logf("[FASTSYNC] VirtualProtect failed: %lu", GetLastError());
                return;
            }
            real_server_sleep = (VOID(WINAPI *)(DWORD))(uintptr_t)iatThunk->u1.Function;
            iatThunk->u1.Function = (uintptr_t)hook_server_sleep;
            g_server_sleep_iat = iatThunk;
            VirtualProtect(&iatThunk->u1.Function, sizeof(iatThunk->u1.Function), oldProtect, &oldProtect);
            logf("[FASTSYNC] Patched server.dll Sleep IAT slot %p (orig %p)", (void *)&iatThunk->u1.Function,
                 (void *)real_server_sleep);
            return;
        }
    }
    logf("[FASTSYNC] server.dll KERNEL32 Sleep import not found — pump throttle left in place");
}

static void restore_server_sleep_iat(void)
{
    if (!g_server_sleep_iat || !real_server_sleep)
        return;

    DWORD oldProtect;
    if (VirtualProtect(&g_server_sleep_iat->u1.Function, sizeof(g_server_sleep_iat->u1.Function), PAGE_READWRITE,
                       &oldProtect))
    {
        g_server_sleep_iat->u1.Function = (uintptr_t)real_server_sleep;
        VirtualProtect(&g_server_sleep_iat->u1.Function, sizeof(g_server_sleep_iat->u1.Function), oldProtect,
                       &oldProtect);
        logf("[FASTSYNC] Restored server.dll Sleep IAT");
    }
    g_server_sleep_iat = NULL;
    real_server_sleep = NULL;
}

/* Best-effort: failure only means the harness guard is off, never fails init. */
static BOOL create_evt_guard_hook(void)
{
    if (!env_flag("HARNESS_EVT_GUARD"))
        return FALSE;

    uint8_t bytes[sizeof(evt_poll_sig)];
    SIZE_T  n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), EVT_POLL_ADDR, bytes, sizeof(bytes), &n) || n != sizeof(bytes) ||
        memcmp(bytes, evt_poll_sig, sizeof(bytes)) != 0)
    {
        logf("[EVT GUARD] Signature mismatch at %p, guard not installed", EVT_POLL_ADDR);
        return FALSE;
    }

    MH_STATUS status = MH_CreateHook(EVT_POLL_ADDR, hook_evt_poll, (void **)&real_evt_poll);
    if (status == MH_OK)
    {
        logf("[EVT GUARD] Installed evt poll NULL guard at %p", EVT_POLL_ADDR);
        return TRUE;
    }
    logf("[EVT GUARD] MH_CreateHook failed: %d", (int)status);
    return FALSE;
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

    // A/B baseline: NETWORKFIX_DISABLE=1 keeps the hooks installed but makes
    // them pass through with the original game semantics (see g_fix_active
    // checks in the hook bodies), so harness fault-injection, tracing and
    // fastsync stay available and only the fix behaviour is toggled.
    g_fix_active = !env_flag("NETWORKFIX_DISABLE");
    if (!g_fix_active)
    {
        logf("[HOOK] NETWORKFIX_DISABLE=1: fix behaviour off (hooks pass through, baseline mode)");
    }

    // Initialize server.dll module (load, detect version, set up ranges).
    // Always done: the ws2 hooks need caller-gating and fastsync needs the base.
    BOOL server_ok = init_server_module();
    if (!server_ok)
    {
        logf("[HOOK] Failed to initialize server module, continuing without network hooks");
    }

    // Initialize MinHook library using MH_Initialize()
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK)
    {
        logf("[HOOK] MH_Initialize failed: %d", (int)status);
        reset_server_globals();
        return FALSE;
    }

    logf("[HOOK] MinHook initialized successfully");

    // Create hooks (installed regardless of fix state; behaviour is gated inside)
    if (server_ok && !create_hooks())
    {
        logf("[HOOK] Some hooks failed to create");
        MH_Uninitialize();
        reset_server_globals();
        return FALSE;
    }

    // Fast sync: unthrottle server.dll's Sleep(30)-paced network pump
    if (server_ok)
    {
        patch_server_sleep_iat();
    }

    // Optional harness-only evt guard (env-gated, independent of network fix)
    // Note: a timeSetEvent tick-boost was tried here to accelerate the paced
    // town-data transfer; any boost (2x, 4x) desyncs the start handshake and
    // the session never launches. The pacing is authentic game behavior.
    BOOL guard_ok = create_evt_guard_hook();

    if (!server_ok && !guard_ok)
    {
        logf("[HOOK] Nothing to hook, MinHook released");
        MH_Uninitialize();
        reset_server_globals();
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
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        restore_server_sleep_iat();
        reset_server_globals();
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

    // Restore the Sleep IAT before FreeLibrary so server.dll is left intact
    // if another module still holds a reference.
    restore_server_sleep_iat();
    reset_server_globals();
    g_HooksInitialized = false;
}
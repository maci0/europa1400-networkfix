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
#include <shlwapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
// SEND_MAX_RETRIES / SEND_RETRY_DELAY_MS live in hooks.h (shared with tests).
#define DEFAULT_SERVER_PATH "Server\\server.dll"

#ifdef NETWORKFIX_TEST
// Test build: internal helpers are externally visible for direct testing.
#define PATH_STATIC
#else
#define PATH_STATIC static
#endif

/* Environment toggles (user and harness). Values are single characters
 * ("0"/"1") or small integers by contract; read once per process by callers. */

/* Numeric value of an environment variable, 0 when unset or empty. */
static int env_int(const char *name)
{
    char value[16] = {0};
    if (GetEnvironmentVariableA(name, value, sizeof(value)) == 0)
        return 0;
    return atoi(value);
}

/* Reads a "1"-valued environment flag (harness contract). */
static BOOL env_flag(const char *name)
{
    return env_int(name) == 1;
}

/* TRUE only when a variable is explicitly set to "0" (opt-out switches). */
static BOOL env_opt_out(const char *name)
{
    char value[2] = {0};
    return GetEnvironmentVariableA(name, value, sizeof(value)) == 1 && value[0] == '0';
}

/* Per-socket options below are (re-)applied idempotently instead of being
 * remembered per handle: winsock recycles SOCKET values after closesocket,
 * so a remember-once table keyed on the handle could suppress the option on
 * a fresh connection that reused a recorded value (Nagle stays on -> the
 * desync this fix targets returns silently). Re-applying per call keeps
 * correctness across reconnects with no retained per-connection state. */

#ifdef NETWORKFIX_TEST
// Test build: real_recv/real_send are externally writable mocks.
// Sleep and message pumping are redirected to counters so retry loops do not
// waste wallclock time or require a win32 message queue.
#define HOOK_STATIC
void test_sleep(DWORD ms);
void test_pump_messages(void);
#define HOOK_SLEEP(ms) test_sleep(ms)
#define HOOK_PUMP_MESSAGES() test_pump_messages()
#else
#define HOOK_STATIC static
#define HOOK_SLEEP(ms) Sleep(ms)

/* Drains this thread's pending window messages. The send-retry loop can run
 * for seconds on the UI thread while a full send buffer drains; without
 * pumping, WM_PAINT and sent messages queue up and the loading/progress
 * dialog appears frozen for minutes (verified via harness/, see README
 * troubleshooting). Dispatching here keeps the dialog responsive; reentrant
 * sends from a dispatched handler recurse safely (loop state is per-call). */
static void pump_pending_messages(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}
#define HOOK_PUMP_MESSAGES() pump_pending_messages()
#endif

// Global state
static BOOL g_HooksInitialized = false;
// When false (NETWORKFIX_DISABLE=1) the ws2/stream hooks are still installed
// (so harness fault-injection, tracing and fastsync keep working) but they
// pass through with the game's original semantics: this is the A/B baseline.
static BOOL g_fix_active = true;
#ifdef NETWORKFIX_TEST
/* Test build: state visible so tests can assert the fallback contract and
 * drive the PE import-table walk directly. */
DWORD            g_server_rva = 0;
HMODULE          g_hServerDll = NULL;
size_t           g_server_size = 0;
static uintptr_t g_server_base = 0;
#else
static DWORD     g_server_rva = 0;
static HMODULE   g_hServerDll = NULL;
static uintptr_t g_server_base = 0;
static size_t    g_server_size = 0;
#endif

// Original function pointers
HOOK_STATIC int(WSAAPI *real_recv)(SOCKET, char *, int, int) = NULL;
HOOK_STATIC int(WSAAPI *real_send)(SOCKET, const char *, int, int) = NULL;

/* Server.dll srv_gameStreamReader function - RVA varies by version */
typedef int(__cdecl *srv_gameStreamReader_t)(int *ctx, int received, int totalLen);
HOOK_STATIC srv_gameStreamReader_t real_srv_gameStreamReader = NULL;

/**
 * Detect server.dll version by calculating its SHA256 hash.
 * Gets the module path and returns the RVA offset using pattern matching.
 *
 * @param file_hash_out Buffer (65 bytes) receiving the lowercase hex SHA256
 *                      of the loaded server.dll; the caller reuses it as the
 *                      post-load hash instead of re-reading the file.
 * @return RVA offset for the target function, or 0 if pattern matching fails
 */
static DWORD detect_server_version(char *file_hash_out /* [65] */)
{
    if (!g_hServerDll)
    {
        log_msg("[HOOK] Invalid server module handle");
        return 0;
    }

    // Get the module file path directly as wide characters
    wchar_t serverPath[MAX_PATH];
    DWORD   pathLen = GetModuleFileNameW(g_hServerDll, serverPath, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH)
    {
        log_msg("[HOOK] Failed to get module file name: %lu", GetLastError());
        return 0;
    }

    // Calculate file hash directly from wide path
    char *fileHash = file_hash_out;
    if (!fileHash || !calculate_file_sha256(serverPath, fileHash, 65))
    {
        log_msg("[HOOK] Failed to calculate SHA256 for server.dll");
        return 0;
    }

    log_msg("[HOOK] server.dll SHA256: %s", fileHash);

    // Try pattern matching first
    DWORD                pattern_rva = 0;
    PATTERN_MATCH_RESULT result = find_srv_gameStreamReader_by_pattern(g_hServerDll, &pattern_rva);

    if (result == PATTERN_MATCH_SUCCESS)
    {
        log_msg("[HOOK] Pattern matcher found srv_gameStreamReader at RVA: 0x%X", pattern_rva);
        return pattern_rva;
    }

    log_msg("[HOOK] Pattern matching failed: %s", pattern_match_result_to_string(result));

    // Fallback to SHA256-based version lookup
    for (int i = 0; known_versions[i].sha256_hash != NULL; i++)
    {
        if (strcmp(fileHash, known_versions[i].sha256_hash) == 0)
        {
            log_msg("[HOOK] Fallback: Detected %s version (RVA: 0x%X)",
                    known_versions[i].version_name, known_versions[i].target_rva);
            return known_versions[i].target_rva;
        }
    }

    log_msg("[HOOK] Unknown server.dll version with hash: %s", fileHash);
    return 0;
}

/**
 * Resets all global server-related variables to their initial state.
 * Used for cleanup on initialization failure.
 */
PATH_STATIC void reset_server_globals(void)
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
 * Gates a game.ini ServerPath value before it is ever handed to LoadLibrary:
 * relative, no "..", must end in ".dll". Containment inside the game
 * directory is checked separately in load_server_dll().
 */
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

/**
 * Copies the directory portion of hModule's own path into out (MAX_PATH).
 * @return TRUE on success, FALSE if the module path is unavailable or truncated
 */
static BOOL get_module_dir(HMODULE hModule, char *out /* [MAX_PATH] */)
{
    DWORD n = GetModuleFileNameA(hModule, out, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return FALSE;
    return PathRemoveFileSpecA(out) != FALSE;
}

/**
 * Loads server.dll from the configured path after path-safety checks.
 *
 * Rejects unsafe paths (is_safe_server_path) and paths whose canonical
 * form leaves the game directory, then LoadLibraryA's the resolved file.
 *
 * @param serverPath Path to server.dll to load
 * @return TRUE if loaded successfully, FALSE on rejection or load error
 */
static BOOL load_server_dll(const char *serverPath)
{
    log_msg("[HOOK] Loading server.dll from: %s", serverPath);
    if (!is_safe_server_path(serverPath))
    {
        log_msg("[HOOK] Rejected unsafe server path: %s", serverPath);
        return FALSE;
    }

    // Verify canonical path stays within game dir before loading
    char module_dir[MAX_PATH] = {0};
    if (get_module_dir(g_hModule, module_dir))
    {
        char combined[MAX_PATH] = {0};
        char canonical[MAX_PATH] = {0};
        char game_dir[MAX_PATH] = {0};
        if (PathCombineA(combined, module_dir, serverPath) &&
            GetFullPathNameA(combined, sizeof(canonical), canonical, NULL) != 0 &&
            GetFullPathNameA(module_dir, sizeof(game_dir), game_dir, NULL) != 0)
        {
            // Prefix match alone accepts C:\GuildExtra; require containment
            if (!path_is_within_dir(canonical, game_dir))
            {
                log_msg("[HOOK] Rejected path outside game dir: %s -> %s", serverPath, canonical);
                return FALSE;
            }
            log_msg("[HOOK] Canonical server path: %s", canonical);
            g_hServerDll = LoadLibraryA(canonical);
            if (!g_hServerDll)
            {
                log_msg("[HOOK] Failed to load server.dll (error: %lu)", GetLastError());
                return FALSE;
            }
            log_msg("[HOOK] Server.dll loaded at %p", (void *)g_hServerDll);
            return TRUE;
        }
    }

    // Module dir unavailable: best effort with the configured relative path
    g_hServerDll = LoadLibraryA(serverPath);
    if (!g_hServerDll)
    {
        DWORD error = GetLastError();
        log_msg("[HOOK] Failed to load server.dll (error: %lu)", error);
        return FALSE;
    }
    // No Sleep(100) — LoadLibrary is synchronous; original race comment not reproducible.
    log_msg("[HOOK] Server.dll loaded at %p", (void *)g_hServerDll);
    return TRUE;
}

/**
 * Best-effort TOCTOU pre-hash of `path` resolved against the module dir.
 * Fills `out` (65 bytes) and returns TRUE when the file could be hashed.
 * Failure is tolerated: the load proceeds with post-load detection only.
 */
static BOOL preload_hash_server_file(const char *path, char out[65])
{
    // Build absolute file path for pre-hash (best-effort)
    char    module_dir[MAX_PATH] = {0};
    char    file_path[MAX_PATH] = {0};
    wchar_t wpath[MAX_PATH];
    if (!get_module_dir(g_hModule, module_dir) || !PathCombineA(file_path, module_dir, path) ||
        MultiByteToWideChar(CP_ACP, 0, file_path, -1, wpath, MAX_PATH) == 0 ||
        !calculate_file_sha256(wpath, out, 65))
    {
        // Tolerated (load proceeds with post-load detection only), but say why
        // the pre-load TOCTOU check is absent from the log.
        log_msg("[HOOK] Pre-load hash unavailable for %s, relying on post-load detection only",
                path);
        return FALSE;
    }
    log_msg("[HOOK] Pre-load SHA256: %s", out);
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
PATH_STATIC BOOL init_server_module(void)
{
    if (g_hServerDll != NULL && g_server_rva != 0 && g_server_base != 0)
    {
        return TRUE; // Already fully initialized
    }

    // Get server path from game.ini or use default
    const char *serverPath = get_server_path_from_ini(g_hModule);
    if (serverPath && !serverPath[0])
        serverPath = NULL; // empty entry counts as absent

    // TOCTOU mitigation: hash the file before LoadLibrary when we have a filesystem path,
    // then verify after load that the mapped image matches (pattern match is primary).
    // If file can't be hashed pre-load, fall back to post-load detection only.
    // Optional allowlist: if hash known, we know it's whitelisted; if not,
    // we still allow load but pattern matcher must succeed post-load.
    char preHash[65] = {0};

    BOOL loaded = FALSE;
    if (serverPath)
    {
        preload_hash_server_file(serverPath, preHash);
        loaded = load_server_dll(serverPath);
    }

    // Documented contract (docs/configuration.md "Default behavior"): a missing,
    // invalid, or unloadable configured ServerPath falls back to
    // DEFAULT_SERVER_PATH instead of aborting initialization.
    if (!loaded && (!serverPath || strcmp(serverPath, DEFAULT_SERVER_PATH) != 0))
    {
        log_msg("[HOOK] Falling back to %s", DEFAULT_SERVER_PATH);
        memset(preHash, 0, sizeof(preHash));
        serverPath = DEFAULT_SERVER_PATH;
        preload_hash_server_file(serverPath, preHash);
        loaded = load_server_dll(serverPath);
    }

    // Load, validate, and detect version (pattern matcher does post-load validation)
    // The hash computed here is a post-LoadLibrary read of the mapped module's
    // file; reuse it as the TOCTOU post-hash instead of hashing a third time.
    char postHash[65] = {0};
    if (!loaded || (g_server_rva = detect_server_version(postHash)) == 0)
    {
        reset_server_globals();
        return FALSE;
    }

    // Verify pre-hash matches post-load hash if we had one (detect TOCTOU replacement)
    // (a computed SHA256 hex string is never empty, so preHash[0] marks presence)
    if (preHash[0] && postHash[0] && strcmp(preHash, postHash) != 0)
    {
        log_msg("[HOOK] Hash mismatch pre/post load: %s != %s — possible replacement, aborting",
                preHash, postHash);
        reset_server_globals();
        return FALSE;
    }

    // Get module information for range checking
    MODULEINFO module_info = {0};
    if (GetModuleInformation(GetCurrentProcess(), g_hServerDll, &module_info, sizeof(module_info)))
    {
        g_server_base = (uintptr_t)module_info.lpBaseOfDll;
        g_server_size = module_info.SizeOfImage;
        log_msg("[HOOK] Server module range: 0x%p - 0x%p (size: 0x%zX)", (void *)g_server_base,
                (void *)(g_server_base + g_server_size), g_server_size);
    }
    else
    {
        log_msg("[HOOK] Failed to get server module info: %lu", GetLastError());
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
        log_msg("[SERVER HOOK] srv_gameStreamReader called with NULL context");
        return -1;
    }

    // Call original function
    int ret = real_srv_gameStreamReader(ctx, received, totalLen);

    // Apply fixes to prevent network instability
    BOOL modified = false;
    if (ctx[SRV_CTX_ERROR_INDEX] < 0)
    {
        log_msg("[SERVER HOOK] srv_gameStreamReader: Fixed negative ctx[%d] (%d -> 0)",
                SRV_CTX_ERROR_INDEX, ctx[SRV_CTX_ERROR_INDEX]);
        ctx[SRV_CTX_ERROR_INDEX] = 0;
        modified = true;
    }

    if (ret < 0)
    {
        log_msg("[SERVER HOOK] srv_gameStreamReader: Fixed negative return value (%d -> 0)", ret);
        ret = 0;
        modified = true;
    }

    if (modified)
    {
        log_msg("[SERVER HOOK] srv_gameStreamReader: received=%d, totalLen=%d, result=%d", received,
                totalLen, ret);
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

    /* Env is immutable for the process lifetime, so concurrent first callers
     * (UI thread + server.dll pump thread) always resolve the same value;
     * the interlocked publish just keeps a single writer per flag instead of
     * an unsynchronized check-then-act data race. */
    static LONG enabled = -1; // -1 unknown, 0 off, 1 on
    LONG        resolved = InterlockedCompareExchange(&enabled, -1, -1);
    if (resolved == -1)
    {
        // Default on; only "0" disables.
        resolved = env_opt_out("NETWORKFIX_NODELAY") ? 0 : 1;
        InterlockedCompareExchange(&enabled, resolved, -1);
    }
    if (resolved == 0)
        return;

    /* Check the live value instead of remembering handles (see note above the
     * env toggles): a recycled handle starts with Nagle on and gets fixed
     * here, while an already-configured socket costs one cheap getsockopt. */
    int one = 1;
    int cur = 0;
    int cbcur = sizeof(cur);
    if (getsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&cur, &cbcur) == 0 && cur == one)
        return; // already enabled on this socket handle
    if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one)) == 0)
        log_msg("[NODELAY] socket=%u TCP_NODELAY enabled (Nagle off)", (unsigned)s);
    else
        log_msg_rate_limited("nodelay_fail", "[NODELAY] socket=%u setsockopt failed: %d",
                             (unsigned)s, WSAGetLastError());
}

/* Harness-only fault injection (HARNESS_TINY_BUFFERS=N): shrink SO_SNDBUF/
 * SO_RCVBUF on each server.dll socket to N bytes the first time we see it, so
 * send() hits WSAEWOULDBLOCK constantly during active play. This reproduces
 * the exact desync the fix targets (the original game does not retry a partial
 * send), without needing host kernel netem. Applied once per socket. */
static void maybe_shrink_buffers(SOCKET s)
{
    /* Interlocked lazy init: same concurrent-first-call reasoning as
     * maybe_set_nodelay above. */
    static LONG tiny = -1; // -1 unknown, 0 off, >0 target bytes
    LONG        resolved = InterlockedCompareExchange(&tiny, -1, -1);
    if (resolved == -1)
    {
        resolved = env_int("HARNESS_TINY_BUFFERS");
        if (resolved < 0)
            resolved = 0;
        InterlockedCompareExchange(&tiny, resolved, -1);
    }
    if (resolved == 0)
        return;

    /* Idempotent re-apply per call, no handle-keyed memory (see the note
     * above maybe_set_nodelay); log rate limited to match. */
    int val = (int)resolved;
    if (setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&val, sizeof(val)) != 0 ||
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&val, sizeof(val)) != 0)
    {
        // Fault injection silently missing would invalidate harness runs.
        log_msg_rate_limited("tiny_buf_fail", "[TINY BUF] socket=%u setsockopt failed: %d",
                             (unsigned)s, WSAGetLastError());
        return;
    }
    log_msg_rate_limited("tiny_buf", "[TINY BUF] socket=%u SO_SNDBUF/SO_RCVBUF set to %d bytes",
                         (unsigned)s, val);
}

/* Harness-only payload tracing (HARNESS_NET_TRACE=1): hex-dump the first bytes
 * of server.dll traffic so protocol stalls can be diagnosed from hook_log. */
static void maybe_trace_payload(const char *dir, const char *buf, int len)
{
    /* Interlocked lazy init: same concurrent-first-call reasoning as
     * maybe_set_nodelay above. */
    static LONG trace_state = -1; // -1 unknown, 0 off, 1 on
    LONG        resolved = InterlockedCompareExchange(&trace_state, -1, -1);
    if (resolved == -1)
    {
        resolved = env_flag("HARNESS_NET_TRACE") ? 1 : 0;
        InterlockedCompareExchange(&trace_state, resolved, -1);
    }
    if (resolved != 1 || !buf || len <= 0)
        return;
    char hex[3 * 48 + 1];
    int  n = len < 48 ? len : 48;
    for (int i = 0; i < n; i++)
        sprintf(hex + i * 3, "%02X ", (unsigned char)buf[i]);
    hex[n * 3] = '\0';
    log_msg("[NET TRACE] %s len=%d: %s", dir, len, hex);
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
    if (!is_caller_from_server((uintptr_t)CALLER_IP()))
    {
        return real_recv(s, buf, len, flags);
    }

    // Log suspicious parameters but don't block - let Windows handle them
    // (Original HarryTheBird version passed all params through directly)
    if (!buf || len <= 0)
    {
        log_msg("[WS2 HOOK] recv: Suspicious parameters: buf=%p, len=%d (hex=0x%08X)", buf, len,
                (unsigned int)len);
    }

    maybe_set_nodelay(s);
    maybe_shrink_buffers(s);
    int result = real_recv(s, buf, len, flags);
    if (result > 0)
    {
        maybe_trace_payload("recv", buf, result);
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
            // Show buffer state when WSAEWOULDBLOCK occurs (rate limited).
            // The probe is an ioctlsocket syscall and would-block is this
            // poll loop's steady state, so gather it only when the rate
            // limiter will actually emit a line (gate reserves the interval);
            // both outcomes report under the same key and cadence.
            if (log_msg_rate_gate("recv_wouldblock"))
            {
                int available = get_available_bytes(s);
                if (available >= 0)
                {
                    log_msg("[WS2 HOOK] recv: WSAEWOULDBLOCK, %d bytes available in buffer",
                            available);
                }
                else
                {
                    log_msg("[WS2 HOOK] recv: WSAEWOULDBLOCK, buffer state unknown");
                }
            }

            // Convert WSAEWOULDBLOCK to 0 for server.dll calls
            WSASetLastError(NO_ERROR);
            return 0;
        }

        log_winsock_error("[WS2 HOOK] recv", s, error);
        // The logging calls above can overwrite the per-thread last error;
        // re-publish the real winsock error so the caller sees what failed.
        WSASetLastError(error);
    }
    else if (result == 0)
    {
        /* A closed connection that the game keeps polling would emit these
         * lines on every call and roll the 50k-line log over within seconds,
         * destroying earlier evidence; the would-block probe above is gated
         * for the same reason. First occurrence per interval always logs. */
        if (log_msg_rate_gate("recv_closed"))
        {
            log_msg("[WS2 HOOK] recv: Connection gracefully closed by peer on socket %u",
                    (unsigned)s);
            log_socket_buffer_info(s);
        }
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
    if (!is_caller_from_server((uintptr_t)CALLER_IP()))
    {
        return real_send(s, buf, len, flags);
    }

    log_msg_rate_limited("send_called",
                         "[WS2 HOOK] send: called from server.dll: socket=%u, len=%d, flags=0x%X",
                         (unsigned)s, len, flags);
    maybe_set_nodelay(s);
    maybe_shrink_buffers(s);
    maybe_trace_payload("send", buf, len);

    // Baseline (fix off): reproduce the original game's single, no-retry send.
    if (!g_fix_active)
    {
        return real_send(s, buf, len, flags);
    }

    // Log suspicious parameters but don't block - let the loop handle them naturally
    // (Original HarryTheBird version: while(total < len) exits immediately if len <= 0)
    if (!buf || len <= 0)
    {
        log_msg("[WS2 HOOK] send: Suspicious parameters: buf=%p, len=%d (hex=0x%08X)", buf, len,
                (unsigned int)len);
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
                log_msg_rate_limited(
                    "send_wouldblock",
                    "[WS2 HOOK] send: WSAEWOULDBLOCK, send buffer likely full (retry %d/%d)",
                    retry_count + 1, SEND_MAX_RETRIES);
                HOOK_SLEEP(SEND_RETRY_DELAY_MS);
                HOOK_PUMP_MESSAGES();
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
            log_msg("[WS2 HOOK] send: Connection closed by peer after %d/%d bytes", total, len);
            return total;
        }

        total += sent;
        retry_count = 0; // Reset retry counter on successful send
    }

    if (retry_count >= SEND_MAX_RETRIES)
    {
        log_msg_rate_limited(
            "send_max_retries",
            "[WS2 HOOK] send: Max retries exceeded, sent %d/%d bytes (send buffer full)", total,
            len);
        log_socket_buffer_info(s);
        WSASetLastError(WSAETIMEDOUT);
        return total > 0 ? total : SOCKET_ERROR;
    }

    return total;
}

/**
 * Removes surrounding quotes from a GetPrivateProfileStringA result in place.
 * @return The effective string length after stripping
 */
static DWORD strip_surrounding_quotes(char *value, DWORD len)
{
    if (len >= 2 && value[0] == '"' && value[len - 1] == '"')
    {
        memmove(value, value + 1, len - 2);
        value[len - 2] = '\0';
        len -= 2;
    }
    return len;
}

/**
 * Reads server path configuration from game.ini file.
 * Looks for "ServerPath" (documented) or "Server" (legacy) key in "[Network]".
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

    if (hModule == NULL)
    {
        log_msg("[CONFIG] Module handle is NULL.");
        return NULL;
    }

    char module_dir[MAX_PATH] = {0};
    char ini_path[MAX_PATH] = {0};
    if (!get_module_dir(hModule, module_dir) || !PathCombineA(ini_path, module_dir, "game.ini"))
    {
        log_msg("[CONFIG] Could not locate game.ini next to module");
        return NULL;
    }

    // Use GetPrivateProfileStringA() to read from INI file
    // Support both ServerPath (documented) and Server (legacy) keys for backwards compat.
    // An empty value (e.g. ServerPath="") counts as absent so callers fall back
    // to the documented default location.
    DWORD len = GetPrivateProfileStringA("Network", "ServerPath",
                                         "", // Default value
                                         serverPath, sizeof(serverPath), ini_path);
    len = strip_surrounding_quotes(serverPath, len);
    if (len == 0)
    {
        len = GetPrivateProfileStringA("Network", "Server",
                                       "", // Fallback legacy key
                                       serverPath, sizeof(serverPath), ini_path);
        len = strip_surrounding_quotes(serverPath, len);
    }

    if (len > 0)
    {
        log_msg("[CONFIG] Read server path from game.ini: %s", serverPath);
        return serverPath;
    }

    log_msg("[CONFIG] Could not find 'ServerPath'/'Server' in '[Network]' section of %s", ini_path);
    return NULL;
}

/**
 * Helper function to create API hooks with consistent logging.
 * Reduces code duplication in hook creation.
 *
 * @param module Module name (L"ws2_32", etc.)
 * @param function Function name to hook (also used as the log name)
 * @param hook_func Hook function pointer
 * @param original_func Pointer to store original function pointer
 * @return TRUE if hook created successfully, FALSE otherwise
 */
static BOOL create_hook_api(const wchar_t *module, const char *function, void *hook_func,
                            void **original_func)
{
    if (!module || !function || !hook_func || !original_func)
    {
        log_msg("[HOOK] Invalid params for %s hook", function ? function : "(null)");
        return FALSE;
    }
    MH_STATUS status = MH_CreateHookApi(module, function, hook_func, original_func);
    if (status == MH_OK)
    {
        log_msg("[HOOK] Created %s hook", function);
        return TRUE;
    }
    else
    {
        log_msg("[HOOK] Failed to create %s hook: %s (%d)", function, MH_StatusToString(status),
                (int)status);
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
static const uint8_t evt_poll_sig[] = {0x53, 0x56, 0x33, 0xDB, 0x33, 0xF6, 0xA1,
                                       0x94, 0x7E, 0x6B, 0x00, 0x03, 0xC6};
static void(__cdecl *real_evt_poll)(void) = NULL;

static void __cdecl hook_evt_poll(void)
{
    if (*EVT_TABLE_PTR == NULL)
    {
        log_msg_rate_limited("evt_guard", "[EVT GUARD] evt table NULL, poll skipped");
        return;
    }
    real_evt_poll();
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
        log_msg_rate_limited("fastsync", "[FASTSYNC] server.dll pump Sleep(30) clamped to 1 ms");
        ms = 1; // keep yielding so the pump thread never busy-spins
    }
    real_server_sleep(ms);
}

/* Bounded compare of a string embedded in the mapped PE image against a
 * literal. Import-table strings are untrusted bytes: a corrupt image may omit
 * the NUL terminator, and strcmp/_stricmp would chase it past the end of the
 * mapping. This requires the whole literal plus its terminator to lie inside
 * the module before any byte is compared, so no read can leave the image. */
static BOOL pe_str_eq(const BYTE *base, size_t size, const char *str, const char *literal,
                      BOOL ignore_case)
{
    size_t str_off = (size_t)((const BYTE *)str - base);
    if (str_off >= size)
        return FALSE;
    size_t literal_len = strlen(literal);
    /* Strictly greater: the NUL terminator must fit inside the module too. */
    if (size - str_off <= literal_len)
        return FALSE;
    int cmp =
        ignore_case ? _strnicmp(str, literal, literal_len) : memcmp(str, literal, literal_len);
    return cmp == 0 && str[literal_len] == '\0';
}

/* Walks server.dll's import table and returns the IAT slot for KERNEL32!Sleep,
 * or NULL when the import is absent or any table bound is inconsistent. Every
 * dereference is bounds-checked against the mapped image so a corrupt or
 * hand-crafted PE can never send this walk outside the module. */
HOOK_STATIC IMAGE_THUNK_DATA *find_kernel32_sleep_thunk(void)
{
    const BYTE             *base = (const BYTE *)g_hServerDll;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (g_server_size < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > g_server_size)
        return NULL;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return NULL;
    const IMAGE_DATA_DIRECTORY *dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    /* The whole descriptor array must fit: imp_end below derives from dir->Size. */
    if (dir->VirtualAddress == 0 || dir->Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
        (size_t)dir->VirtualAddress + dir->Size > g_server_size)
        return NULL;

    const IMAGE_IMPORT_DESCRIPTOR *imp =
        (const IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress);
    const IMAGE_IMPORT_DESCRIPTOR *imp_end = imp + dir->Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (; imp < imp_end && imp->Name != 0; imp++)
    {
        if ((size_t)imp->Name >= g_server_size)
            continue;
        const char *dllName = (const char *)(base + imp->Name);
        if (!pe_str_eq(base, g_server_size, dllName, "KERNEL32.dll", TRUE))
            continue;
        if (imp->OriginalFirstThunk == 0 || imp->FirstThunk == 0)
            continue;
        if ((size_t)imp->OriginalFirstThunk + sizeof(IMAGE_THUNK_DATA) > g_server_size ||
            (size_t)imp->FirstThunk + sizeof(IMAGE_THUNK_DATA) > g_server_size)
            continue;
        const IMAGE_THUNK_DATA *nameThunk =
            (const IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iatThunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; nameThunk->u1.AddressOfData != 0; nameThunk++, iatThunk++)
        {
            /* The NUL terminator is untrusted data; stop before either thunk
             * array would leave the module. */
            if ((size_t)((const BYTE *)nameThunk - base) + sizeof(IMAGE_THUNK_DATA) >
                    g_server_size ||
                (size_t)((BYTE *)iatThunk - base) + sizeof(IMAGE_THUNK_DATA) > g_server_size)
            {
                break;
            }
            if (nameThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            if ((size_t)nameThunk->u1.AddressOfData + sizeof(IMAGE_IMPORT_BY_NAME) > g_server_size)
                continue;
            const IMAGE_IMPORT_BY_NAME *ibn =
                (const IMAGE_IMPORT_BY_NAME *)(base + nameThunk->u1.AddressOfData);
            if (pe_str_eq(base, g_server_size, (const char *)ibn->Name, "Sleep", FALSE))
            {
                return iatThunk;
            }
        }
        break; // KERNEL32.dll appears at most once in an import table
    }
    return NULL;
}

static void patch_server_sleep_iat(void)
{
    if (env_opt_out("NETWORKFIX_FASTSYNC"))
    {
        log_msg("[FASTSYNC] Disabled via NETWORKFIX_FASTSYNC=0");
        return;
    }

    if (!g_hServerDll || g_server_size == 0)
        return;

    IMAGE_THUNK_DATA *thunk = find_kernel32_sleep_thunk();
    if (!thunk)
    {
        log_msg("[FASTSYNC] KERNEL32 Sleep import not found, pump throttle left in place");
        return;
    }

    DWORD oldProtect;
    if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), PAGE_READWRITE,
                        &oldProtect))
    {
        log_msg("[FASTSYNC] VirtualProtect failed: %lu", GetLastError());
        return;
    }
    real_server_sleep = (VOID(WINAPI *)(DWORD))(uintptr_t)thunk->u1.Function;
    thunk->u1.Function = (uintptr_t)hook_server_sleep;
    g_server_sleep_iat = thunk;
    if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function), oldProtect, &oldProtect))
    {
        // Page stays writable for the process lifetime; surface it.
        log_msg("[FASTSYNC] Failed to restore IAT page protection: %lu", GetLastError());
    }
    log_msg("[FASTSYNC] Patched server.dll Sleep IAT slot %p (orig %p)",
            (void *)&thunk->u1.Function, (void *)real_server_sleep);
}

static void restore_server_sleep_iat(void)
{
    if (!g_server_sleep_iat || !real_server_sleep)
        return;

    /* This IAT patch is not MinHook-managed, so MH_DisableHook/MH_Uninitialize
     * never freeze threads executing hook_server_sleep. A pump thread that
     * entered the hook just before the swap below resumes afterwards and reads
     * these globals again, so they must stay valid: kernel32!Sleep lives for
     * the process lifetime, hence the pointers are kept (never NULLed) and
     * this function stays idempotent if cleanup runs twice. */
    DWORD oldProtect;
    if (VirtualProtect(&g_server_sleep_iat->u1.Function, sizeof(g_server_sleep_iat->u1.Function),
                       PAGE_READWRITE, &oldProtect))
    {
        g_server_sleep_iat->u1.Function = (uintptr_t)real_server_sleep;
        if (!VirtualProtect(&g_server_sleep_iat->u1.Function,
                            sizeof(g_server_sleep_iat->u1.Function), oldProtect, &oldProtect))
        {
            // Page stays writable for the process lifetime; surface it.
            log_msg("[FASTSYNC] Failed to restore IAT page protection: %lu", GetLastError());
        }
        log_msg("[FASTSYNC] Restored server.dll Sleep IAT");
    }
    else
    {
        // Without write access the IAT keeps pointing at hook_server_sleep; if
        // server.dll stays mapped via another reference while this DLL unloads,
        // its pump thread would jump into freed code. Never skip silently.
        log_msg("[FASTSYNC] VirtualProtect failed before IAT restore: %lu — hook left installed",
                GetLastError());
    }
}

/* Best-effort: failure only means the harness guard is off, never fails init. */
static BOOL create_evt_guard_hook(void)
{
    if (!env_flag("HARNESS_EVT_GUARD"))
        return FALSE;

    uint8_t bytes[sizeof(evt_poll_sig)];
    SIZE_T  n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), EVT_POLL_ADDR, bytes, sizeof(bytes), &n) ||
        n != sizeof(bytes) || memcmp(bytes, evt_poll_sig, sizeof(bytes)) != 0)
    {
        log_msg("[EVT GUARD] Signature mismatch at %p, guard not installed", EVT_POLL_ADDR);
        return FALSE;
    }

    MH_STATUS status = MH_CreateHook(EVT_POLL_ADDR, hook_evt_poll, (void **)&real_evt_poll);
    if (status == MH_OK)
    {
        log_msg("[EVT GUARD] Installed evt poll NULL guard at %p", EVT_POLL_ADDR);
        return TRUE;
    }
    log_msg("[EVT GUARD] MH_CreateHook failed: %s (%d)", MH_StatusToString(status), (int)status);
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
        log_msg("[HOOK] Server module not properly initialized - cannot create server hook");
        success = FALSE;
    }
    else
    {
        void     *targetAddr = (void *)((uintptr_t)g_hServerDll + g_server_rva);
        MH_STATUS status = MH_CreateHook(targetAddr, hook_srv_gameStreamReader,
                                         (void **)&real_srv_gameStreamReader);
        if (status == MH_OK)
        {
            log_msg("[HOOK] Created hook for server function at %p (RVA +0x%X)", targetAddr,
                    g_server_rva);
        }
        else
        {
            log_msg("[HOOK] Failed to create hook for server function: %s (%d)",
                    MH_StatusToString(status), (int)status);
            success = FALSE;
        }
    }

    // Create API hooks using helper function
    success &= create_hook_api(L"ws2_32", "recv", hook_recv, (void **)&real_recv);
    success &= create_hook_api(L"ws2_32", "send", hook_send, (void **)&real_send);

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

    log_msg("[HOOK] Initialization started (PID: %lu, TID: %lu)", GetCurrentProcessId(),
            GetCurrentThreadId());

    // A/B baseline: NETWORKFIX_DISABLE=1 keeps the hooks installed but makes
    // them pass through with the original game semantics (see g_fix_active
    // checks in the hook bodies), so harness fault-injection, tracing and
    // fastsync stay available and only the fix behaviour is toggled.
    g_fix_active = !env_flag("NETWORKFIX_DISABLE");
    if (!g_fix_active)
    {
        log_msg(
            "[HOOK] NETWORKFIX_DISABLE=1: fix behaviour off (hooks pass through, baseline mode)");
    }

    // Initialize server.dll module (load, detect version, set up ranges).
    // Always done: the ws2 hooks need caller-gating and fastsync needs the base.
    BOOL server_ok = init_server_module();
    if (!server_ok)
    {
        log_msg("[HOOK] Failed to initialize server module, continuing without network hooks");
    }

    // Initialize MinHook library using MH_Initialize()
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK)
    {
        log_msg("[HOOK] MH_Initialize failed: %s (%d)", MH_StatusToString(status), (int)status);
        reset_server_globals();
        return FALSE;
    }

    log_msg("[HOOK] MinHook initialized successfully");

    // Create hooks (installed regardless of fix state; behaviour is gated inside)
    if (server_ok && !create_hooks())
    {
        log_msg("[HOOK] Some hooks failed to create");
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
        log_msg("[HOOK] Nothing to hook, MinHook released");
        MH_Uninitialize();
        reset_server_globals();
        return FALSE;
    }

    // Enable all hooks using MH_EnableHook()
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status == MH_OK)
    {
        log_msg("[HOOK] All hooks enabled successfully");
        g_HooksInitialized = true;
    }
    else
    {
        log_msg("[HOOK] Failed to enable hooks: %s (%d)", MH_StatusToString(status), (int)status);
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        restore_server_sleep_iat();
        reset_server_globals();
        return FALSE;
    }

    log_msg("[HOOK] Initialization completed successfully");
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

    log_msg("[HOOK] Cleanup started");

    MH_STATUS disableStatus = MH_DisableHook(MH_ALL_HOOKS);
    MH_STATUS uninitStatus = MH_Uninitialize();

    log_msg("[HOOK] Cleanup completed (Disable: %d, Uninit: %d)", (int)disableStatus,
            (int)uninitStatus);

    // Restore the Sleep IAT before FreeLibrary so server.dll is left intact
    // if another module still holds a reference.
    restore_server_sleep_iat();
    reset_server_globals();
    g_HooksInitialized = false;
}
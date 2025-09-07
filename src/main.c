#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "MinHook.h"
#include "logging.h"

// Link required libraries
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "psapi.lib")

// Configuration constants
static const char *kServerPath = "Server\\server.dll";

// Thread synchronization and module tracking
static bool g_HooksInitialized = false;

static uintptr_t g_ServerBase = 0;
static size_t g_ServerSize = 0;

static void InitServerModuleRange(void) {
    if (g_ServerBase != 0) return;

    HMODULE hServer = GetModuleHandleA("server.dll");
    if (!hServer) {
        logf("[HOOK] server.dll not found in process");
        return;
    }

    MODULEINFO mi = {0};
    if (GetModuleInformation(GetCurrentProcess(), hServer, &mi, sizeof(mi))) {
        g_ServerBase = (uintptr_t)mi.lpBaseOfDll;
        g_ServerSize = (size_t)mi.SizeOfImage;
        logf("[HOOK] server.dll mapped at %p, size: 0x%zx", (void*)g_ServerBase, g_ServerSize);
    } else {
        logf("[HOOK] Failed to get server.dll module info: %lu", GetLastError());
    }
}

static BOOL IsCallerFromServer(void) {
    if (g_ServerBase == 0) {
        InitServerModuleRange();
        if (g_ServerBase == 0) return FALSE;
    }

    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_CONTROL;
    RtlCaptureContext(&ctx);

    uintptr_t callerAddr;
#if defined(_M_X64) || defined(__x86_64__)
    callerAddr = ctx.Rip;
#elif defined(_M_IX86) || defined(__i386__)
    callerAddr = ctx.Eip;
#else
#error "Unsupported architecture"
#endif

    BOOL inRange = (callerAddr >= g_ServerBase && callerAddr < g_ServerBase + g_ServerSize);

    if (inRange) {
        logf("[HOOK] Caller from server.dll: 0x%p (offset +0x%zx)",
             (void*)callerAddr, (size_t)(callerAddr - g_ServerBase));
    }

    return inRange;
}

/* -------- Original function pointers -------- */
static int (WINAPI *real_recv)(SOCKET, char*, int, int) = NULL;
static int (WINAPI *real_send)(SOCKET, const char*, int, int) = NULL;
static DWORD (WINAPI *real_GetTickCount)(void) = NULL;

/* Server.dll target function @ RVA 0x3720 */
typedef int (WINAPI *pF3720_t)(int *ctx, int received, int totalLen);
static pF3720_t real_F3720 = NULL;

/* -------- Hook implementations -------- */

DWORD WINAPI hook_GetTickCount(void) {
    DWORD result = real_GetTickCount ? real_GetTickCount() : GetTickCount64() & 0xFFFFFFFF;
    return result;
}

static int WINAPI hook_F3720(int *ctx, int received, int totalLen) {
    // Validate parameters
    if (!ctx) {
        logf("[SERVER HOOK] F3720 called with NULL context");
        return -1;
    }

    // Call original function
    int ret = real_F3720(ctx, received, totalLen);

    // Apply fixes
    bool modified = false;
    // The server can sometimes set a negative value to this field, which causes
    // instability. This fix ensures the value is never negative.
    if (ctx[0xE] < 0) {
        logf("[SERVER HOOK] F3720: Fixed negative ctx[0xE] (%d -> 0)", ctx[0xE]);
        ctx[0xE] = 0;
        modified = true;
    }

    if (ret < 0) {
        logf("[SERVER HOOK] F3720: Fixed negative return value (%d -> 0)", ret);
        ret = 0;
        modified = true;
    }

    if (modified) {
        logf("[SERVER HOOK] F3720: received=%d, totalLen=%d, result=%d",
             received, totalLen, ret);
    }

    return ret;
}

int WINAPI hook_recv(SOCKET s, char *buf, int len, int flags) {
    // Only intercept calls from server.dll
    if (!IsCallerFromServer()) {
        return real_recv(s, buf, len, flags);
    }

    logf("[WS2 HOOK] recv called from server.dll: socket=%u, len=%d, flags=0x%X",
         (unsigned)s, len, flags);

    // Validate parameters
    if (!buf || len <= 0) {
        logf("[WS2 HOOK] recv: Invalid parameters");
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }

    int result = real_recv(s, buf, len, flags);

    if (result == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            // Convert WSAEWOULDBLOCK to 0 for server.dll calls
            logf("[WS2 HOOK] recv: WSAEWOULDBLOCK converted to 0 (server.dll caller)");
            WSASetLastError(NO_ERROR);
            return 0;
        }

        log_winsock_error("[WS2 HOOK] recv", s, error);
    } else if (result == 0) {
        logf("[WS2 HOOK] recv: Connection gracefully closed by peer on socket %u", (unsigned)s);
    } else {
        logf("[WS2 HOOK] recv: Success, received %d bytes", result);
    }

    return result;
}

int WINAPI hook_send(SOCKET s, const char *buf, int len, int flags) {
    // Only intercept calls from server.dll
    if (!IsCallerFromServer()) {
        return real_send(s, buf, len, flags);
    }

    logf("[WS2 HOOK] send called from server.dll: socket=%u, len=%d, flags=0x%X",
         (unsigned)s, len, flags);

    // Validate parameters
    if (!buf || len <= 0) {
        logf("[WS2 HOOK] send: Invalid parameters");
        WSASetLastError(WSAEINVAL);
        return SOCKET_ERROR;
    }

    int total = 0;
    int retry_count = 0;
    const int MAX_RETRIES = 1000; // Prevent infinite loops

    while (total < len && retry_count < MAX_RETRIES) {
        int sent = real_send(s, buf + total, len - total, flags);

        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                logf("[WS2 HOOK] send: WSAEWOULDBLOCK, retrying... (%d/%d)",
                     retry_count + 1, MAX_RETRIES);
                Sleep(1);
                retry_count++;
                continue;
            }

            log_winsock_error("[WS2 HOOK] send", s, error);
            WSASetLastError(error);
            if (error == WSAECONNRESET || error == WSAECONNABORTED) {
                return total > 0 ? total : SOCKET_ERROR;
            }
            return SOCKET_ERROR;
        }

        if (sent == 0) {
            logf("[WS2 HOOK] send: Connection closed by peer after %d/%d bytes",
                 total, len);
            return total;
        }

        total += sent;
        retry_count = 0; // Reset retry counter on successful send
    }

    if (retry_count >= MAX_RETRIES) {
        logf("[WS2 HOOK] send: Max retries exceeded, sent %d/%d bytes", total, len);
        WSASetLastError(WSAETIMEDOUT);
        return total > 0 ? total : SOCKET_ERROR;
    }

    logf("[WS2 HOOK] send: Success, sent %d bytes total", total);
    return total;
}

/* -------- Initialization -------- */

static bool CreateHooks(void) {
    MH_STATUS status;
    bool success = true;

    // Get server path from environment variable or use default
    const char *serverPath = getenv("EUROPA1400_SERVER_PATH");
    if (!serverPath) {
        serverPath = kServerPath;
    }

    // Load server.dll and hook the target function
    HMODULE hServer = LoadLibraryA(serverPath);
    if (!hServer) {
        DWORD error = GetLastError();
        logf("[HOOK] Failed to load %s (error: %lu)", serverPath, error);
    } else {
        logf("[HOOK] Loaded %s at %p", serverPath, (void*)hServer);

        void *addrF3720 = (void*)((uintptr_t)hServer + 0x3720);
        status = MH_CreateHook(addrF3720, hook_F3720, (void**)&real_F3720);
        if (status == MH_OK) {
            logf("[HOOK] Created hook for F3720 at %p", addrF3720);
        } else {
            logf("[HOOK] Failed to create hook for F3720: %d", (int)status);
            success = false;
        }
    }

    // Hook Winsock functions
    status = MH_CreateHookApi(L"ws2_32", "recv", hook_recv, (void**)&real_recv);
    if (status == MH_OK) {
        logf("[HOOK] Created recv hook");
    } else {
        logf("[HOOK] Failed to create recv hook: %d", (int)status);
        success = false;
    }

    status = MH_CreateHookApi(L"ws2_32", "send", hook_send, (void**)&real_send);
    if (status == MH_OK) {
        logf("[HOOK] Created send hook");
    } else {
        logf("[HOOK] Failed to create send hook: %d", (int)status);
        success = false;
    }

    // Hook GetTickCount
    status = MH_CreateHookApi(L"kernel32", "GetTickCount", hook_GetTickCount, (void**)&real_GetTickCount);
    if (status == MH_OK) {
        logf("[HOOK] Created GetTickCount hook");
    } else {
        logf("[HOOK] Failed to create GetTickCount hook: %d", (int)status);
        success = false;
    }

    return success;
}

static DWORD WINAPI InitThread(LPVOID lpParam) {
    logf("[HOOK] Initialization started (PID: %lu, TID: %lu)",
         GetCurrentProcessId(), GetCurrentThreadId());

    // Initialize server.dll module range
    InitServerModuleRange();

    // Initialize MinHook
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        logf("[HOOK] MH_Initialize failed: %d", (int)status);
        return 1;
    }

    logf("[HOOK] MinHook initialized successfully");

    // Create all hooks
    if (!CreateHooks()) {
        logf("[HOOK] Some hooks failed to create");
    }

    // Enable all hooks
    status = MH_EnableHook(MH_ALL_HOOKS);
    if (status == MH_OK) {
        logf("[HOOK] All hooks enabled successfully");
        g_HooksInitialized = true;
    } else {
        logf("[HOOK] Failed to enable hooks: %d", (int)status);
        return 1;
    }

    // Smoke test
    DWORD tickCount = GetTickCount();
    logf("[HOOK] GetTickCount test: %lu", tickCount);

    logf("[HOOK] Initialization completed successfully");
    return 0;
}

static void CleanupHooks(void) {
    if (!g_HooksInitialized) {
        return;
    }

    logf("[HOOK] Cleanup started");

    MH_STATUS disableStatus = MH_DisableHook(MH_ALL_HOOKS);
    MH_STATUS uninitStatus = MH_Uninitialize();

    logf("[HOOK] Cleanup completed (Disable: %d, Uninit: %d)",
         (int)disableStatus, (int)uninitStatus);

    g_HooksInitialized = false;
}

/* -------- DLL Entry Point -------- */

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    switch (dwReason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);

            if (!init_logging(hModule)) {
                OutputDebugStringA("[HOOK] Failed to initialize logging. Aborting attach.\n");
                return FALSE;
            }

            // Create initialization thread
            HANDLE hThread = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
            if (hThread) {
                CloseHandle(hThread);
            } else {
                logf("[HOOK] Failed to create initialization thread");
                close_logging();
                return FALSE;
            }
            break;

        case DLL_PROCESS_DETACH:
            logf("[HOOK] DLL detaching from process");

            CleanupHooks();

            close_logging();
            break;

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            // Nothing to do for thread attach/detach
            break;
    }

    return TRUE;
}

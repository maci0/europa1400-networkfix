#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include "MinHook.h"

// Link required libraries
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")

// Configuration constants
static const char *kServerPath = "Server\\server.dll";
static const DWORD kMaxLogLines = 500;  // Max lines before rollover

// Thread synchronization and module tracking
static CRITICAL_SECTION g_LogCriticalSection;
static bool g_CriticalSectionInitialized = false;
static bool g_HooksInitialized = false;
static HANDLE g_LogFile = INVALID_HANDLE_VALUE;
static UINT32 g_LogLineCount = 0;

// Server.dll module range for caller filtering
static uintptr_t g_ServerBase = 0;
static size_t g_ServerSize = 0;

/* -------- Utility functions -------- */

static void ResetLogFile(void) {
    if (g_LogFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(g_LogFile, 0, NULL, FILE_BEGIN);
        SetEndOfFile(g_LogFile);
        g_LogLineCount = 0;
    }
}

static void logf(const char *fmt, ...) {
    if (!g_CriticalSectionInitialized || g_LogFile == INVALID_HANDLE_VALUE) return;

    EnterCriticalSection(&g_LogCriticalSection);

    // Check for log rollover
    if (++g_LogLineCount > kMaxLogLines) {
        ResetLogFile();
    }

    // Format timestamp
    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[64];
    snprintf(timestamp, sizeof(timestamp), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // Write timestamp
    DWORD written;
    WriteFile(g_LogFile, timestamp, (DWORD)strlen(timestamp), &written, NULL);

    // Format and write message
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buffer, sizeof(buffer) - 2, fmt, ap);  // Leave space for \r\n
    va_end(ap);

    if (len > 0) {
        // Ensure we have newline
        if (len < sizeof(buffer) - 2 && buffer[len-1] != '\n') {
            buffer[len++] = '\n';
            buffer[len] = '\0';
        }
        WriteFile(g_LogFile, buffer, len, &written, NULL);
        FlushFileBuffers(g_LogFile);  // Ensure immediate write
    }

    LeaveCriticalSection(&g_LogCriticalSection);
}

static void InitServerModuleRange(void) {
    if (g_ServerBase != 0) return;  // Already initialized

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

    // Capture the caller's address
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

static const char* GetErrorDescription(int errorCode) {
    switch (errorCode) {
        case WSAECONNRESET: return "WSAECONNRESET";
        case WSAECONNABORTED: return "WSAECONNABORTED";
        case WSAENETDOWN: return "WSAENETDOWN";
        case WSAETIMEDOUT: return "WSAETIMEDOUT";
        case WSAENOTCONN: return "WSAENOTCONN";
        case WSAEWOULDBLOCK: return "WSAEWOULDBLOCK";
        case WSAEINVAL: return "WSAEINVAL";
        case WSAENOTSOCK: return "WSAENOTSOCK";
        case WSAEFAULT: return "WSAEFAULT";
        case WSAEINTR: return "WSAEINTR";
        case WSAEMSGSIZE: return "WSAEMSGSIZE";
        default: return "UNKNOWN";
    }
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
        switch (error) {
            case WSAEWOULDBLOCK:
                // Convert WSAEWOULDBLOCK to 0 for server.dll calls
                logf("[WS2 HOOK] recv: WSAEWOULDBLOCK converted to 0 (server.dll caller)");
                WSASetLastError(NO_ERROR);
                return 0;

            case WSAECONNRESET:
                logf("[WS2 HOOK] recv: Connection reset by peer (WSAECONNRESET) on socket %u", (unsigned)s);
                break;

            case WSAECONNABORTED:
                logf("[WS2 HOOK] recv: Connection aborted (WSAECONNABORTED) on socket %u", (unsigned)s);
                break;

            case WSAENETDOWN:
                logf("[WS2 HOOK] recv: Network subsystem failure (WSAENETDOWN) on socket %u", (unsigned)s);
                break;

            case WSAETIMEDOUT:
                logf("[WS2 HOOK] recv: Connection timeout (WSAETIMEDOUT) on socket %u", (unsigned)s);
                break;

            case WSAENOTCONN:
                logf("[WS2 HOOK] recv: Socket not connected (WSAENOTCONN) on socket %u", (unsigned)s);
                break;

            default:
                logf("[WS2 HOOK] recv: Error %d (%s) on socket %u",
                     error, GetErrorDescription(error), (unsigned)s);
                break;
        }
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
            switch (error) {
                case WSAEWOULDBLOCK:
                    logf("[WS2 HOOK] send: WSAEWOULDBLOCK, retrying... (%d/%d)",
                         retry_count + 1, MAX_RETRIES);
                    Sleep(1);
                    retry_count++;
                    continue;

                case WSAECONNRESET:
                    logf("[WS2 HOOK] send: Connection reset by peer after sending %d/%d bytes",
                         total, len);
                    WSASetLastError(error);
                    return total > 0 ? total : SOCKET_ERROR;

                case WSAECONNABORTED:
                    logf("[WS2 HOOK] send: Connection aborted after sending %d/%d bytes",
                         total, len);
                    WSASetLastError(error);
                    return total > 0 ? total : SOCKET_ERROR;

                default:
                    logf("[WS2 HOOK] send: Fatal error %d (%s) after sending %d/%d bytes",
                         error, GetErrorDescription(error), total, len);
                    WSASetLastError(error);
                    return SOCKET_ERROR;
            }
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

    // Load server.dll and hook the target function
    HMODULE hServer = LoadLibraryA(kServerPath);
    if (!hServer) {
        DWORD error = GetLastError();
        logf("[HOOK] Failed to load %s (error: %lu)", kServerPath, error);
    } else {
        logf("[HOOK] Loaded %s at %p", kServerPath, (void*)hServer);

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

            // Initialize critical section for logging
            InitializeCriticalSection(&g_LogCriticalSection);
            g_CriticalSectionInitialized = true;

            // Create log file in DLL directory
            wchar_t dllPath[MAX_PATH];
            GetModuleFileNameW(hModule, dllPath, MAX_PATH);
            PathRemoveFileSpecW(dllPath);
            wcscat_s(dllPath, MAX_PATH, L"\\hook_log.txt");

            g_LogFile = CreateFileW(
                dllPath,
                GENERIC_WRITE,
                FILE_SHARE_READ,
                NULL,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                NULL
            );

            if (g_LogFile != INVALID_HANDLE_VALUE) {
                SetFilePointer(g_LogFile, 0, NULL, FILE_END);
                logf("[HOOK] DLL attached to process %lu, log: %ls", GetCurrentProcessId(), dllPath);
            }

            // Create initialization thread
            HANDLE hThread = CreateThread(NULL, 0, InitThread, NULL, 0, NULL);
            if (hThread) {
                CloseHandle(hThread);
            } else {
                logf("[HOOK] Failed to create initialization thread");
                return FALSE;
            }
            break;

        case DLL_PROCESS_DETACH:
            logf("[HOOK] DLL detaching from process");

            CleanupHooks();

            // Cleanup log file and critical section
            if (g_LogFile != INVALID_HANDLE_VALUE) {
                CloseHandle(g_LogFile);
                g_LogFile = INVALID_HANDLE_VALUE;
            }

            if (g_CriticalSectionInitialized) {
                DeleteCriticalSection(&g_LogCriticalSection);
                g_CriticalSectionInitialized = false;
            }
            break;

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            // Nothing to do for thread attach/detach
            break;
    }

    return TRUE;
}

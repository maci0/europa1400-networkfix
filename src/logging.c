#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>
#include <shlwapi.h>

#include "logging.h"

CRITICAL_SECTION g_LogCriticalSection;
bool g_CriticalSectionInitialized = false;
HANDLE g_LogFile = INVALID_HANDLE_VALUE;
UINT32 g_LogLineCount = 0;

static const DWORD kMaxLogLines = 500;  // Max lines before rollover

static void ResetLogFile(void) {
    if (g_LogFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(g_LogFile, 0, NULL, FILE_BEGIN);
        SetEndOfFile(g_LogFile);
        g_LogLineCount = 0;
    }
}

void logf(const char *fmt, ...) {
    if (!g_CriticalSectionInitialized || g_LogFile == INVALID_HANDLE_VALUE) return;

    EnterCriticalSection(&g_LogCriticalSection);

    if (++g_LogLineCount > kMaxLogLines) {
        ResetLogFile();
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[64];
    snprintf(timestamp, sizeof(timestamp), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    DWORD written;
    WriteFile(g_LogFile, timestamp, (DWORD)strlen(timestamp), &written, NULL);

    va_list ap;
    va_start(ap, fmt);
    int len = _vscprintf(fmt, ap);
    va_end(ap);

    if (len > 0) {
        char *buffer = (char*)malloc(len + 2); // +2 for newline and null terminator
        if (buffer) {
            va_start(ap, fmt);
            vsnprintf(buffer, len + 1, fmt, ap);
            va_end(ap);

            if (buffer[len-1] != '\n') {
                buffer[len++] = '\n';
                buffer[len] = '\0';
            }

            WriteFile(g_LogFile, buffer, len, &written, NULL);
            FlushFileBuffers(g_LogFile);
            free(buffer);
        }
    }

    LeaveCriticalSection(&g_LogCriticalSection);
}

static char* GetErrorDescription(int errorCode) {
    char *buffer = NULL;
    DWORD result = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        errorCode,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buffer,
        0,
        NULL
    );

    if (result == 0) {
        // Fallback for unknown errors
        const char *unknown = "Unknown error";
        buffer = (char*)LocalAlloc(LPTR, strlen(unknown) + 1);
        if (buffer) {
            strcpy(buffer, unknown);
        }
    }

    return buffer;
}

void log_winsock_error(const char *prefix, SOCKET s, int error) {
    char *description = GetErrorDescription(error);
    if (description) {
        logf("%s: %s (%d) on socket %u", prefix, description, error, (unsigned)s);
        LocalFree(description);
    } else {
        logf("%s: Unknown error (%d) on socket %u", prefix, error, (unsigned)s);
    }
}

bool init_logging(HMODULE hModule) {
    InitializeCriticalSection(&g_LogCriticalSection);
    g_CriticalSectionInitialized = true;

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

    if (g_LogFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    SetFilePointer(g_LogFile, 0, NULL, FILE_END);
    logf("[HOOK] DLL attached to process %lu, log: %ls", GetCurrentProcessId(), dllPath);
    return true;
}

void close_logging(void) {
    if (g_LogFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_LogFile);
        g_LogFile = INVALID_HANDLE_VALUE;
    }

    if (g_CriticalSectionInitialized) {
        DeleteCriticalSection(&g_LogCriticalSection);
        g_CriticalSectionInitialized = false;
    }
}


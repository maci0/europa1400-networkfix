#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>

#include "logging.h"

static const DWORD kMaxLogLines = 500;  // Max lines before rollover

CRITICAL_SECTION g_LogCriticalSection;
bool g_CriticalSectionInitialized = false;
HANDLE g_LogFile = INVALID_HANDLE_VALUE;
UINT32 g_LogLineCount = 0;

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

    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buffer, sizeof(buffer) - 2, fmt, ap);
    va_end(ap);

    if (len > 0) {
        if (len < (int)sizeof(buffer) - 2 && buffer[len-1] != '\n') {
            buffer[len++] = '\n';
            buffer[len] = '\0';
        }
        WriteFile(g_LogFile, buffer, len, &written, NULL);
        FlushFileBuffers(g_LogFile);
    }

    LeaveCriticalSection(&g_LogCriticalSection);
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

void log_winsock_error(const char *prefix, SOCKET s, int error) {
    logf("%s: %s (%d) on socket %u", prefix,
         GetErrorDescription(error), error, (unsigned)s);
}


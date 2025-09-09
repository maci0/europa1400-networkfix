/*
 * logging.c: A simple file-based logging implementation.
 */

#define WIN32_LEAN_AND_MEAN
#include <shlwapi.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <winsock2.h>

#include "logging.h"

logging_context       g_logctx = {0};

static const uint32_t max_log_lines = 50000u; /* Max lines before rollover */

// Resets the log file by truncating it to zero length.
// This is called when the log file exceeds the maximum number of lines.
static void reset_log_file(void)
{
    if (g_logctx.log_file != INVALID_HANDLE_VALUE)
    {
        SetFilePointer(g_logctx.log_file, 0, NULL, FILE_BEGIN);
        SetEndOfFile(g_logctx.log_file);
        g_logctx.log_line_count = 0;
    }
}

// Writes a formatted string to the log file.
// This function is thread-safe.
void logf(const char *fmt, ...)
{
    if (!g_logctx.critical_section_initialized || g_logctx.log_file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    EnterCriticalSection(&g_logctx.critical_section);

    if (++g_logctx.log_line_count > max_log_lines)
    {
        reset_log_file();
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[64];
    int  timestamp_len = snprintf(timestamp, sizeof(timestamp), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ", st.wYear,
                                  st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    char buffer[1024];
    memcpy(buffer, timestamp, timestamp_len);

    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buffer + timestamp_len, sizeof(buffer) - timestamp_len, fmt, ap);
    va_end(ap);

    if (len > 0)
    {
        if (buffer[timestamp_len + len - 1] != '\n')
        {
            buffer[timestamp_len + len] = '\n';
            buffer[timestamp_len + len + 1] = '\0';
            len++;
        }

        DWORD written;
        WriteFile(g_logctx.log_file, buffer, timestamp_len + len, &written, NULL);
        FlushFileBuffers(g_logctx.log_file);
    }

    LeaveCriticalSection(&g_logctx.critical_section);
}

static char *GetErrorDescription(int errorCode)
{
    char *buffer = NULL;
    DWORD result =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buffer, 0, NULL);

    if (result == 0)
    {
        // Fallback for unknown errors
        const char *unknown = "Unknown error";
        buffer = (char *)LocalAlloc(LPTR, strlen(unknown) + 1);
        if (buffer)
        {
            strcpy(buffer, unknown);
        }
    }

    return buffer;
}

void log_winsock_error(const char *prefix, SOCKET s, int error)
{
    char *description = GetErrorDescription(error);
    if (description)
    {
        logf("%s: %s (%d) on socket %u", prefix, description, error, (unsigned)s);
        LocalFree(description);
    }
    else
    {
        logf("%s: Unknown error (%d) on socket %u", prefix, error, (unsigned)s);
    }
}

bool init_logging(HMODULE hModule)
{
    InitializeCriticalSection(&g_logctx.critical_section);
    g_logctx.critical_section_initialized = true;

    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(hModule, dllPath, MAX_PATH);
    PathRemoveFileSpecW(dllPath);
    wcscat_s(dllPath, MAX_PATH, L"\\hook_log.txt");

    g_logctx.log_file =
        CreateFileW(dllPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (g_logctx.log_file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    SetFilePointer(g_logctx.log_file, 0, NULL, FILE_END);
    logf("[HOOK] DLL attached to process %lu, log: %ls", GetCurrentProcessId(), dllPath);
    return true;
}

void close_logging(void)
{
    if (g_logctx.log_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_logctx.log_file);
        g_logctx.log_file = INVALID_HANDLE_VALUE;
    }

    if (g_logctx.critical_section_initialized)
    {
        DeleteCriticalSection(&g_logctx.critical_section);
        g_logctx.critical_section_initialized = false;
    }
}

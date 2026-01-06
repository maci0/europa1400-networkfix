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

logging_context g_logctx = {0};

// Logging constants
static const uint32_t MAX_LOG_LINES = 50000u;     /* Max lines before rollover */
static const size_t   LOG_BUFFER_SIZE = 2048;     /* Log message buffer size */
static const size_t   TIMESTAMP_BUFFER_SIZE = 64; /* Timestamp buffer size */

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

    if (++g_logctx.log_line_count > MAX_LOG_LINES)
    {
        reset_log_file();
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[TIMESTAMP_BUFFER_SIZE];
    int  timestamp_len = snprintf(timestamp, sizeof(timestamp), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ", st.wYear,
                                  st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // Validate timestamp length
    if (timestamp_len < 0 || timestamp_len >= (int)sizeof(timestamp))
    {
        return; // Timestamp formatting failed
    }

    char buffer[LOG_BUFFER_SIZE];
    // Bounds-checked copy of timestamp
    if (timestamp_len >= (int)sizeof(buffer))
    {
        return; // Timestamp too long for buffer
    }
    memcpy(buffer, timestamp, timestamp_len);

    va_list ap;
    va_start(ap, fmt);
    int remaining_space = (int)sizeof(buffer) - timestamp_len;
    int len = vsnprintf(buffer + timestamp_len, remaining_space, fmt, ap);
    va_end(ap);

    // Handle vsnprintf return value correctly
    if (len < 0)
    {
        return; // Formatting error
    }
    if (len >= remaining_space)
    {
        len = remaining_space - 1; // Truncated
    }

    if (len > 0)
    {
        // Check if we need to add newline and have space for it
        int total_len = timestamp_len + len;
        if (buffer[total_len - 1] != '\n' && total_len < (int)sizeof(buffer) - 1)
        {
            buffer[total_len] = '\n';
            buffer[total_len + 1] = '\0';
            len++;
        }

        DWORD written;
        WriteFile(g_logctx.log_file, buffer, timestamp_len + len, &written, NULL);
        // Removed FlushFileBuffers for better performance
        // FlushFileBuffers(g_logctx.log_file);
    }
    LeaveCriticalSection(&g_logctx.critical_section);
}

static char *GetErrorDescription(int errorCode)
{
    char *buffer = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                   errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buffer, 0, NULL);

    // Returns NULL if FormatMessageA fails - caller handles this case
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

void log_socket_buffer_info(SOCKET s)
{
    static SOCKET last_logged_socket = INVALID_SOCKET;

    if (s == last_logged_socket)
    {
        return;
    }

    // Get socket buffer sizes
    int recv_buf_size = -1;
    int send_buf_size = -1;
    int opt_len = sizeof(int);

    getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&recv_buf_size, &opt_len);
    opt_len = sizeof(int);
    getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&send_buf_size, &opt_len);

    logf("[WS2 HOOK] Socket %u: recv_buf=%d, send_buf=%d", (unsigned)s, recv_buf_size, send_buf_size);
    last_logged_socket = s;
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
        // Flush any remaining data before closing
        FlushFileBuffers(g_logctx.log_file);
        CloseHandle(g_logctx.log_file);
        g_logctx.log_file = INVALID_HANDLE_VALUE;
    }

    if (g_logctx.critical_section_initialized)
    {
        DeleteCriticalSection(&g_logctx.critical_section);
        g_logctx.critical_section_initialized = false;
    }
}

/**
 * Rate-limited logging function to prevent spam.
 * Only logs a message if it hasn't been logged recently.
 */
void logf_rate_limited(const char *key, const char *fmt, ...)
{
    static struct
    {
        char  key[64];
        DWORD last_logged;
    } rate_limit_cache[10] = {0};

    DWORD current_time = GetTickCount();
    int   cache_slot = -1;

    // Find existing entry or empty slot
    for (int i = 0; i < 10; i++)
    {
        if (strcmp(rate_limit_cache[i].key, key) == 0)
        {
            cache_slot = i;
            break;
        }
        if (cache_slot == -1 && rate_limit_cache[i].key[0] == '\0')
        {
            cache_slot = i;
        }
    }

    // Use oldest entry if no empty slot found
    if (cache_slot == -1)
    {
        cache_slot = 0;
        DWORD oldest_time = rate_limit_cache[0].last_logged;
        for (int i = 1; i < 10; i++)
        {
            if (rate_limit_cache[i].last_logged < oldest_time)
            {
                oldest_time = rate_limit_cache[i].last_logged;
                cache_slot = i;
            }
        }
    }

    // Check if enough time has passed
    if (current_time - rate_limit_cache[cache_slot].last_logged < LOG_RATE_LIMIT_MS)
    {
        return; // Skip logging
    }

    // Update cache and log message
    strncpy(rate_limit_cache[cache_slot].key, key, sizeof(rate_limit_cache[cache_slot].key) - 1);
    rate_limit_cache[cache_slot].key[sizeof(rate_limit_cache[cache_slot].key) - 1] = '\0';
    rate_limit_cache[cache_slot].last_logged = current_time;

    va_list ap;
    va_start(ap, fmt);
    char buffer[512];
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    logf("%s", buffer);
}
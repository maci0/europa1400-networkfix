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

logging_context g_logctx = {.log_file = INVALID_HANDLE_VALUE};

// Logging constants
static const uint32_t MAX_LOG_LINES = 50000u;     /* Max lines before rollover */
static const size_t   LOG_BUFFER_SIZE = 2048;     /* Log message buffer size */
static const size_t   TIMESTAMP_BUFFER_SIZE = 64; /* Timestamp buffer size */

/* Last socket whose buffer sizes were logged; dedups repeated log lines. */
static SOCKET s_last_logged_socket = INVALID_SOCKET;

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
void log_msg(const char *fmt, ...)
{
    // Fast path (unlocked read): skip the section entirely when logging is down.
    if (!g_logctx.critical_section_initialized || g_logctx.log_file == INVALID_HANDLE_VALUE)
    {
        return;
    }

    EnterCriticalSection(&g_logctx.critical_section);

    // Re-check under the section: close_logging() may have invalidated the
    // file handle between the fast-path check and acquiring the section, and
    // writing to a closed (or recycled) handle would corrupt unrelated state.
    if (!g_logctx.critical_section_initialized || g_logctx.log_file == INVALID_HANDLE_VALUE)
    {
        LeaveCriticalSection(&g_logctx.critical_section);
        return;
    }

    if (++g_logctx.log_line_count > MAX_LOG_LINES)
    {
        reset_log_file();
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char timestamp[TIMESTAMP_BUFFER_SIZE];
    int  timestamp_len =
        snprintf(timestamp, sizeof(timestamp), "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ", st.wYear,
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    // Validate timestamp length
    if (timestamp_len < 0 || timestamp_len >= (int)sizeof(timestamp))
    {
        LeaveCriticalSection(&g_logctx.critical_section);
        return; // Timestamp formatting failed
    }

    char buffer[LOG_BUFFER_SIZE];
    memcpy(buffer, timestamp, timestamp_len);

    va_list ap;
    va_start(ap, fmt);
    int remaining_space = (int)sizeof(buffer) - timestamp_len;
    int len = vsnprintf(buffer + timestamp_len, remaining_space, fmt, ap);
    va_end(ap);

    // Handle vsnprintf return value correctly
    if (len < 0)
    {
        LeaveCriticalSection(&g_logctx.critical_section);
        return; // Formatting error
    }
    if (len >= remaining_space)
    {
        len = remaining_space - 1; // Truncated
    }

    if (len > 0)
    {
        // Ensure the record always ends its own line, even when the message
        // filled the buffer exactly: there the NUL sits on the final byte and
        // must give way to the newline (WriteFile needs no terminator).
        int total_len = timestamp_len + len;
        if (buffer[total_len - 1] != '\n')
        {
            if (total_len < (int)sizeof(buffer) - 1)
            {
                buffer[total_len] = '\n';
                buffer[total_len + 1] = '\0';
            }
            else
            {
                /* Exact-fit truncation: the NUL sits on the final byte; swap
                 * it for the newline so this record cannot merge with the
                 * next one in the file. */
                buffer[total_len] = '\n';
            }
            len++;
        }

        DWORD written;
        DWORD to_write = (DWORD)(timestamp_len + len);
        if (!WriteFile(g_logctx.log_file, buffer, to_write, &written, NULL) || written != to_write)
        {
            /* A failed or short write means every later message is lost with
             * no trace (disk full, handle invalidated). Announce once on the
             * debug channel so the loss is at least visible; never spam and
             * never recurse into log_msg from inside its own lock. */
            static bool write_failure_reported = false;
            if (!write_failure_reported)
            {
                write_failure_reported = true;
                char ods[128];
                int  n = snprintf(ods, sizeof(ods),
                                  "[HOOK] log write failed/short (%lu of %lu, error %lu); "
                                  "further log lines may be lost\n",
                                  (unsigned long)written, (unsigned long)to_write, GetLastError());
                if (n > 0)
                    OutputDebugStringA(ods);
            }
        }
        // Removed FlushFileBuffers for better performance
        // FlushFileBuffers(g_logctx.log_file);
    }
    LeaveCriticalSection(&g_logctx.critical_section);
}

static char *GetErrorDescription(int errorCode)
{
    char *buffer = NULL;
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buffer, 0, NULL);

    // cppcheck-suppress knownConditionTrueFalse
    // False positive: FORMAT_MESSAGE_ALLOCATE_BUFFER writes through &buffer,
    // which cppcheck cannot model; it assumes buffer stays NULL.
    if (buffer)
    {
        // FormatMessageA appends "\r\n"; trim it so one log entry stays one
        // line (log_msg would otherwise only append its newline conditionally).
        size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r'))
            buffer[--len] = '\0';
        if (len == 0)
        {
            LocalFree(buffer);
            buffer = NULL;
        }
    }

    // Returns NULL if FormatMessageA fails or yields only whitespace - caller handles this case
    return buffer;
}

void log_winsock_error(const char *prefix, SOCKET s, int error)
{
    char *description = GetErrorDescription(error);
    // cppcheck-suppress knownConditionTrueFalse
    // False positive: FORMAT_MESSAGE_ALLOCATE_BUFFER writes through &buffer,
    // which cppcheck cannot model; it assumes buffer stays NULL.
    if (description)
    {
        log_msg("%s: %s (%d) on socket %u", prefix, description, error, (unsigned)s);
        LocalFree(description);
    }
    else
    {
        log_msg("%s: Unknown error (%d) on socket %u", prefix, error, (unsigned)s);
    }
}

void log_socket_buffer_info(SOCKET s)
{
    BOOL should_log = FALSE;

    if (!g_logctx.critical_section_initialized)
    {
        // Logging not ready: skip dedup, log directly (log_msg will no-op)
        should_log = TRUE;
    }
    else
    {
        EnterCriticalSection(&g_logctx.critical_section);
        if (s != s_last_logged_socket)
        {
            s_last_logged_socket = s;
            should_log = TRUE;
        }
        LeaveCriticalSection(&g_logctx.critical_section);
    }

    if (!should_log)
        return;

    // Get socket buffer sizes (outside lock, slow syscall)
    int  recv_buf_size = -1;
    int  send_buf_size = -1;
    int  opt_len = sizeof(int);

    BOOL recv_ok = getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&recv_buf_size, &opt_len) == 0;
    int  recv_err = WSAGetLastError(); // capture before the next call overwrites it
    opt_len = sizeof(int);
    BOOL send_ok = getsockopt(s, SOL_SOCKET, SO_SNDBUF, (char *)&send_buf_size, &opt_len) == 0;
    int  send_err = WSAGetLastError();

    if (recv_ok && send_ok)
    {
        log_msg("[WS2 HOOK] Socket %u: recv_buf=%d, send_buf=%d", (unsigned)s, recv_buf_size,
                send_buf_size);
    }
    else
    {
        /* A failed probe must not read like real buffer sizes (-1): report
         * which query failed and why so the socket state stays debuggable. */
        if (!recv_ok)
            log_winsock_error("[WS2 HOOK] Socket buffer probe (SO_RCVBUF)", s, recv_err);
        if (!send_ok)
            log_winsock_error("[WS2 HOOK] Socket buffer probe (SO_SNDBUF)", s, send_err);
    }
}

bool init_logging(HMODULE hModule)
{
    InitializeCriticalSection(&g_logctx.critical_section);
    g_logctx.critical_section_initialized = true;
    g_logctx.log_file = INVALID_HANDLE_VALUE;

    if (hModule == NULL)
    {
        close_logging();
        return false;
    }

    wchar_t dllPath[MAX_PATH];
    DWORD   n = GetModuleFileNameW(hModule, dllPath, MAX_PATH);
    // n >= MAX_PATH means the path was truncated (and may be unterminated).
    if (n == 0 || n >= MAX_PATH)
    {
        close_logging();
        return false;
    }
    if (!PathRemoveFileSpecW(dllPath))
    {
        close_logging();
        return false;
    }
    if (wcscat_s(dllPath, MAX_PATH, L"\\hook_log.txt") != 0)
    {
        close_logging();
        return false;
    }

    g_logctx.log_file = CreateFileW(dllPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, NULL);

    if (g_logctx.log_file == INVALID_HANDLE_VALUE)
    {
        close_logging();
        return false;
    }

    SetFilePointer(g_logctx.log_file, 0, NULL, FILE_END);
    log_msg("[HOOK] DLL attached to process %lu, log: %ls", GetCurrentProcessId(), dllPath);
    return true;
}

void close_logging(void)
{
    /* Invalidate the file handle under the section so a concurrent log_msg
     * either skips or finishes its write before we close the stale handle
     * outside the section. */
    EnterCriticalSection(&g_logctx.critical_section);
    HANDLE file = g_logctx.log_file;
    g_logctx.log_file = INVALID_HANDLE_VALUE;
    g_logctx.critical_section_initialized = false;
    LeaveCriticalSection(&g_logctx.critical_section);

    if (file != INVALID_HANDLE_VALUE)
    {
        // Flush any remaining data before closing
        FlushFileBuffers(file);
        CloseHandle(file);
    }

    /* The critical section is deliberately never deleted. On a real
     * FreeLibrary unload game threads are not joined first, so a logger can
     * still be inside or queued on the section here; deleting an in-use
     * CRITICAL_SECTION is undefined behavior. Leaking one fixed-size section
     * per process keeps those late callers safe: they acquire it normally,
     * see the invalidated handle and return without writing. */
}

/* Rate-limit slot table shared by log_msg_rate_limited and log_msg_rate_gate.
 * Distinct keys tracked at once; overflow evicts the oldest. Headroom above
 * the ten keys currently used by hooks.c. */
#define LOG_RATE_LIMIT_SLOTS 16

static struct
{
    char      key[64];
    ULONGLONG last_logged;
} rate_limit_cache[LOG_RATE_LIMIT_SLOTS] = {0};

/* Resolves the cache slot for `key` (existing entry, first free slot, or
 * oldest-entry eviction) and, when the per-key interval has elapsed, reserves
 * the slot by recording key and timestamp. Caller holds the critical section.
 * @return true when exactly one message under `key` may be written now */
static bool rate_limit_acquire(const char *key, ULONGLONG current_time)
{
    int cache_slot = -1;

    // Find existing entry or empty slot
    for (int i = 0; i < LOG_RATE_LIMIT_SLOTS; i++)
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
        ULONGLONG oldest_time = rate_limit_cache[0].last_logged;
        for (int i = 1; i < LOG_RATE_LIMIT_SLOTS; i++)
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
        return false; // Skip logging

    // Reserve the slot for the caller's one message
    strncpy(rate_limit_cache[cache_slot].key, key, sizeof(rate_limit_cache[cache_slot].key) - 1);
    rate_limit_cache[cache_slot].key[sizeof(rate_limit_cache[cache_slot].key) - 1] = '\0';
    rate_limit_cache[cache_slot].last_logged = current_time;
    return true;
}

bool log_msg_rate_gate(const char *key)
{
    /* The limiter itself, without formatting: hot paths use it to skip
     * gathering diagnostics (syscalls, buffer dumps) for lines that would be
     * suppressed anyway. A true return reserves the interval, so the follow-up
     * message must be emitted unconditionally. */
    if (!key || !g_logctx.critical_section_initialized)
        return false;

    // Elapsed-time measurement: GetTickCount64 never wraps (32-bit
    // GetTickCount wraps after ~49.7 days uptime, corrupting both the
    // interval check and the oldest-slot eviction in rate_limit_acquire).
    ULONGLONG current_time = GetTickCount64();

    EnterCriticalSection(&g_logctx.critical_section);
    bool due = rate_limit_acquire(key, current_time);
    LeaveCriticalSection(&g_logctx.critical_section);
    return due;
}

/**
 * Rate-limited logging function to prevent spam.
 * Only logs a message if it hasn't been logged recently.
 */
void log_msg_rate_limited(const char *key, const char *fmt, ...)
{
    if (!log_msg_rate_gate(key))
        return;

    va_list ap;
    va_start(ap, fmt);
    char buffer[512];
    int  len = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    // Formatting failure leaves buffer indeterminate; never pass it on.
    if (len < 0)
        return;

    log_msg("%s", buffer);
}

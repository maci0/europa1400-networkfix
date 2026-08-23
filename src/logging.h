#ifndef LOGGING_H
#define LOGGING_H

// winsock2.h must precede windows.h (mingw warns otherwise)
#include <stdbool.h>
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>

typedef struct
{
    CRITICAL_SECTION critical_section;
    bool             critical_section_initialized;
    HANDLE           log_file;
    UINT32           log_line_count;
} logging_context;

#define LOG_RATE_LIMIT_MS 5000 // Rate limit same messages to once per 5 seconds

extern logging_context g_logctx;

bool                   init_logging(HMODULE hModule);
void                   close_logging(void);
void                   log_msg(const char *fmt, ...);
void                   log_msg_rate_limited(const char *key, const char *fmt, ...);
void                   log_winsock_error(const char *prefix, SOCKET s, int error);
void                   log_socket_buffer_info(SOCKET s);

#endif // LOGGING_H

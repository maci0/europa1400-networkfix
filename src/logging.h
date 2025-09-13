#ifndef LOGGING_H
#define LOGGING_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include <winsock2.h>

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
void                   logf(const char *fmt, ...);
void                   logf_rate_limited(const char *key, const char *fmt, ...);
void                   log_winsock_error(const char *prefix, SOCKET s, int error);
void                   log_socket_buffer_info(SOCKET s);

#endif // LOGGING_H

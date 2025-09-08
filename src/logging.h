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

extern logging_context g_logctx;

bool                   init_logging(HMODULE hModule);
void                   close_logging(void);
void                   logf(const char *fmt, ...);
void                   log_winsock_error(const char *prefix, SOCKET s, int error);

#endif // LOGGING_H

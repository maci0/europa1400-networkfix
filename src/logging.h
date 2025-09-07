#ifndef LOGGING_H
#define LOGGING_H

#include <winsock2.h>
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

static CRITICAL_SECTION g_LogCriticalSection;
static bool g_CriticalSectionInitialized = false;
static HANDLE g_LogFile = INVALID_HANDLE_VALUE;
static UINT32 g_LogLineCount = 0;

void init_logging(HMODULE hModule);
void close_logging(void);
void logf(const char *fmt, ...);
void log_winsock_error(const char *prefix, SOCKET s, int error);

#endif // LOGGING_H

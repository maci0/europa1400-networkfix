#ifndef LOGGING_H
#define LOGGING_H

#include <winsock2.h>
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    CRITICAL_SECTION g_LogCriticalSection;
    bool g_CriticalSectionInitialized;
    HANDLE g_LogFile;
    UINT32 g_LogLineCount;
} LoggingContext;

extern LoggingContext g_LoggingContext;

bool init_logging(HMODULE hModule);
void close_logging(void);
void logf(const char *fmt, ...);
void log_winsock_error(const char *prefix, SOCKET s, int error);

#endif // LOGGING_H

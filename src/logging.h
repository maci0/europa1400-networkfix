#ifndef LOGGING_H
#define LOGGING_H

#include <winsock2.h>
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>

extern CRITICAL_SECTION g_LogCriticalSection;
extern bool g_CriticalSectionInitialized;
extern HANDLE g_LogFile;
extern UINT32 g_LogLineCount;

bool init_logging(HMODULE hModule);
void close_logging(void);
void logf(const char *fmt, ...);
void log_winsock_error(const char *prefix, SOCKET s, int error);

#endif // LOGGING_H

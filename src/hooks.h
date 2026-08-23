#ifndef HOOKS_H
#define HOOKS_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include <winsock2.h>

// Cap retries to avoid hanging the game thread forever on a persistently
// full send buffer (peer not reading). 5000 * 1ms ~ 5 s before we give
// up with WSAETIMEDOUT, long enough for normal VPN jitter, finite
// enough to avoid an INT_MAX busy-loop that overflows signed int.
#define SEND_MAX_RETRIES 5000
#define SEND_RETRY_DELAY_MS 1 // Delay between send retries (matches original)

// Index of the error-code field inside srv_gameStreamReader's stream context
// struct (offset 0x38). Negative values there desync the network stream; the
// hook clamps them to 0.
#define SRV_CTX_ERROR_INDEX 0xE

// Hook initialization and cleanup functions
BOOL init_hooks(void);
void cleanup_hooks(void);

// Server module range detection
BOOL is_caller_from_server(uintptr_t caller_addr);
#ifdef NETWORKFIX_TEST
extern BOOL g_test_force_caller_server;
BOOL        is_safe_server_path(const char *path);
BOOL        path_is_within_dir(const char *path, const char *dir);
#endif

// Hook implementations
int WSAAPI   hook_recv(SOCKET s, char *buf, int len, int flags);
int WSAAPI   hook_send(SOCKET s, const char *buf, int len, int flags);
DWORD WINAPI hook_GetTickCount(void);
int __cdecl  hook_srv_gameStreamReader(int *ctx, int received, int totalLen);

// Configuration
const char *get_server_path_from_ini(HMODULE hModule);

// Global module handle (defined in main.c)
extern HMODULE g_hModule;

#endif // HOOKS_H
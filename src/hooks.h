#ifndef HOOKS_H
#define HOOKS_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>
#include <winsock2.h>

// Hook initialization and cleanup functions
BOOL init_hooks(void);
void cleanup_hooks(void);

// Server module range detection
BOOL is_caller_from_server(uintptr_t caller_addr);

// Hook implementations
int WSAAPI   hook_recv(SOCKET s, char *buf, int len, int flags);
int WSAAPI   hook_send(SOCKET s, const char *buf, int len, int flags);
DWORD WINAPI hook_GetTickCount(void);
int __cdecl  hook_srv_gameStreamReader(int *ctx, int received, int totalLen);

// Configuration
const char *get_server_path_from_ini(HMODULE hModule);

#endif // HOOKS_H
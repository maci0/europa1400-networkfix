#ifndef HOOKS_H
#define HOOKS_H

// winsock2.h must precede windows.h (mingw warns otherwise)
#include "sha256.h"
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>

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

// Server.dll srv_gameStreamReader function pointer type; the RVA of the
// target function varies by game version (see versions.c/pattern_matcher.c).
typedef int(__cdecl *srv_gameStreamReader_t)(int *ctx, int received, int totalLen);

#ifdef NETWORKFIX_TEST
extern BOOL g_test_force_caller_server;
/* Internals exposed so the Wine test suite can drive them directly. */
BOOL is_safe_server_path(const char *path);
BOOL path_is_within_dir(const char *path, const char *dir);
BOOL server_hash_mismatch(const char pre[SHA256_HEX_SIZE], const char post[SHA256_HEX_SIZE]);
IMAGE_THUNK_DATA *find_kernel32_sleep_thunk(void);
BOOL              init_server_module(void);
void              reset_server_globals(void);
extern HMODULE    g_hServerDll;
extern DWORD      g_server_rva;
extern size_t     g_server_size;
/* A/B baseline switch (NETWORKFIX_DISABLE) and the socket option it gates. */
extern BOOL g_fix_active;
void        maybe_set_nodelay(SOCKET s);
/* Original-function trampolines; writable mocks in the test build. */
extern int(WSAAPI *real_recv)(SOCKET, char *, int, int);
extern int(WSAAPI *real_send)(SOCKET, const char *, int, int);
extern srv_gameStreamReader_t real_srv_gameStreamReader;
#endif

// Hook implementations
int WSAAPI  hook_recv(SOCKET s, char *buf, int len, int flags);
int WSAAPI  hook_send(SOCKET s, const char *buf, int len, int flags);
int __cdecl hook_srv_gameStreamReader(int *ctx, int received, int totalLen);

// Configuration
const char *get_server_path_from_ini(HMODULE hModule);

// Global module handle (defined in main.c)
extern HMODULE g_hModule;

#endif // HOOKS_H

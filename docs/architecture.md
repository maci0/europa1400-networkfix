# Architecture Documentation

This document provides a technical overview of the europa1400-networkfix architecture, explaining how the plugin intercepts and modifies game behavior at runtime.

## Table of Contents

- [Overview](#overview)
- [Plugin Loading Mechanism](#plugin-loading-mechanism)
- [Hook Architecture](#hook-architecture)
- [Module Components](#module-components)
- [Hook Implementation Details](#hook-implementation-details)
- [Version Detection](#version-detection)
- [Error Handling Strategy](#error-handling-strategy)
- [Performance Considerations](#performance-considerations)
- [Security Considerations](#security-considerations)
- [Future Improvements](#future-improvements)
- [Harness (xdotool + Lua)](#harness-xdotool--lua)
- [References](#references)

## Overview

The europa1400-networkfix is a runtime API hook plugin that intercepts Windows networking functions and game-specific functions to add resilient error handling and retry logic. It operates without modifying any game files on disk.

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                      Europa 1400 Process                        │
│                                                                 │
│  ┌────────────────┐      ┌──────────────────────────────────┐  │
│  │  Game Engine   │      │    networkfix.asi Plugin         │  │
│  │                │      │                                  │  │
│  │  ┌──────────┐  │      │  ┌────────────────────────────┐ │  │
│  │  │server.dll│◄─┼──────┼──┤  Hook Layer (MinHook)      │ │  │
│  │  └──────────┘  │      │  └────────────────────────────┘ │  │
│  │                │      │           │                      │  │
│  │       │        │      │           ▼                      │  │
│  └───────┼────────┘      │  ┌────────────────────────────┐ │  │
│          │               │  │  Network Logic & Retry     │ │  │
│          ▼               │  └────────────────────────────┘ │  │
│  ┌────────────────┐      │           │                      │  │
│  │  ws2_32.dll    │◄─────┼───────────┘                      │  │
│  │  (Winsock)     │      │                                  │  │
│  └────────────────┘      └──────────────────────────────────┘  │
│          │                                                      │
└──────────┼──────────────────────────────────────────────────────┘
           ▼
    Network Layer
```

## Plugin Loading Mechanism

### ASI Plugin Loader

On a native Windows Gold Edition install the game's ASI loader picks up `.asi` files from the game directory at startup. Under Wine (and in `harness/`) that loader never fires: Miles only scans ASI providers when audio init succeeds, which it does not headless. The harness therefore loads `networkfix.asi` via dxwrapper (`[Plugins] LoadPlugins=1` + `WINEDLLOVERRIDES=…;d3d8=n,b`).

### Load Sequence

1. Game process starts (`Europa1400Gold_TL.exe`)
2. ASI loader discovers `networkfix.asi` in game directory
3. Windows loads the DLL into game process address space
4. `DllMain` is called with `DLL_PROCESS_ATTACH`
5. Plugin initialization begins

### DllMain Entry Point

[src/main.c:63-114](../src/main.c#L63-L114) (abridged; the real code checks every
return value and fails attach when logging or the init thread cannot start):

```c
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved)
{
    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:
        // 1. Store module handle
        g_hModule = hModule;

        // 2. Disable thread callbacks (performance optimization)
        DisableThreadLibraryCalls(hModule);

        // 3. Initialize logging system
        init_logging(hModule);

        // 4. Create initialization thread (avoids DllMain deadlocks)
        CreateThread(NULL, 0, init_thread, NULL, 0, NULL);
        break;

    case DLL_PROCESS_DETACH:
        // Join the init thread and tear down only on a real FreeLibrary
        // (lpReserved == NULL). Process-exit detach skips waits/frees.
        if (lpReserved == NULL)
        {
            cleanup_hooks();
            close_logging();
        }
        break;
    }
    return TRUE;
}
```

**Why a separate initialization thread?**

DllMain has strict limitations - it cannot safely acquire locks or load other DLLs. Creating a separate thread allows us to safely initialize MinHook and load `server.dll` without deadlock risks.

## Hook Architecture

### MinHook Library

The plugin uses [MinHook](https://github.com/TsudaKageyu/minhook), a minimalistic x86/x64 API hooking library that:

- Creates inline hooks (x86 assembly trampolines)
- Preserves original function behavior
- Handles thread safety
- Supports multiple hooks simultaneously

### Hook Installation Process

[src/hooks.c](../src/hooks.c)

```c
BOOL init_hooks(void)
{
    if (g_HooksInitialized) return TRUE;
    g_fix_active = !env_flag("NETWORKFIX_DISABLE"); // hooks stay installed; behaviour gated inside
    BOOL server_ok = init_server_module();          // always: caller-gating + fastsync need the base
    if (MH_Initialize() != MH_OK) return FALSE;
    if (server_ok && !create_hooks()) return FALSE; // MH_CreateHook(server RVA) + MH_CreateHookApi
    if (server_ok) patch_server_sleep_iat();        // NETWORKFIX_FASTSYNC=0 skips the IAT write
    BOOL guard_ok = create_evt_guard_hook();        // HARNESS_EVT_GUARD=1, signature-checked
    if (!server_ok && !guard_ok) return FALSE;
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) return FALSE;
    g_HooksInitialized = true;
    return TRUE;
}
```

### Hook Chain

When a hooked function is called:

1. **Original call** - Game code calls `recv()`
2. **Trampoline** - MinHook redirects to `hook_recv()`
3. **Hook logic** - Our code processes/modifies behavior
4. **Original function** - We call the real `recv()` via MinHook
5. **Return path** - Result flows back through hook to game

```
Game Code → recv() → [MinHook Trampoline] → hook_recv()
                                                  │
                                                  ├─ Add retry logic
                                                  ├─ Handle WSAEWOULDBLOCK
                                                  └─ Call real_recv()
                                                         │
                                                         └─ Return to game
```

## Module Components

### 1. Main Module ([src/main.c](../src/main.c))

**Responsibilities:**
- DLL entry point and lifecycle management
- Initialization thread creation
- Module handle storage

**Key Functions:**
- `DllMain()` - Windows DLL entry point
- `init_thread()` - Background initialization worker

### 2. Hook Module ([src/hooks.c](../src/hooks.c), [src/hooks.h](../src/hooks.h))

**Responsibilities:**
- Hook creation and management
- Network function interception
- Server.dll function hooking
- Caller detection for selective hooking

**Key Functions:**
- `init_hooks()` - Initialize all hooks
- `hook_recv()` - Winsock receive hook
- `hook_send()` - Winsock send hook
- `hook_srv_gameStreamReader()` - Server.dll packet validation hook
- `is_caller_from_server()` - Detects if caller is from server.dll
- `patch_server_sleep_iat()` / `restore_server_sleep_iat()` - Fastsync Sleep(30)→1 ms
- `maybe_set_nodelay()` - TCP_NODELAY on server.dll sockets

### 3. Logging Module ([src/logging.c](../src/logging.c), [src/logging.h](../src/logging.h))

**Responsibilities:**
- File-based logging to `hook_log.txt`
- Thread-safe log writing
- Initialization status reporting

**Key Functions:**
- `init_logging()` - Open log file
- `log_msg()` - Write formatted log message
- `close_logging()` - Close and flush log file

### 4. Pattern Matcher ([src/pattern_matcher.c](../src/pattern_matcher.c), [src/pattern_matcher.h](../src/pattern_matcher.h))

**Responsibilities:**
- Binary pattern search for unknown server.dll versions
- Instruction pattern matching for function signatures

**Key Functions:**
- `find_srv_gameStreamReader_by_pattern()`: scan `server.dll` for the common prologue
- `find_first_valid_match()`: scan every pattern occurrence, accept the first that validates (a false-positive hit can no longer mask a valid match later in the image)
- `find_pattern_in_memory()` / `validate_function_prologue()`: mask search plus JE/JNE bounds check (test-only exports)

### 5. Version Detection ([src/versions.c](../src/versions.c), [src/sha256.c](../src/sha256.c))

**Responsibilities:**
- Detect server.dll version by SHA256 hash
- Map version to correct function offset (RVA)

**Key Constants:**
- Known SHA256 hashes for Steam/GOG versions
- RVA offsets for packet validation function

## Hook Implementation Details

### recv() Hook - Handling Non-Blocking Socket Errors

[src/hooks.c](../src/hooks.c) - `hook_recv()`

**Problem:** Game treats `WSAEWOULDBLOCK` as fatal error instead of "no data available yet"

**Solution:**
```c
int WSAAPI hook_recv(SOCKET s, char *buf, int len, int flags)
{
    // Call original recv function
    int result = real_recv(s, buf, len, flags);

    // Check for would-block error
    if (result == SOCKET_ERROR)
    {
        int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK)
        {
            // Convert to graceful "no data" instead of error
            WSASetLastError(0);
            return 0;  // No data available, try again later
        }
    }

    return result;
}
```

**Impact:** Game can now poll non-blocking sockets without crashing on normal "no data" conditions.

### send() Hook - Retry Logic for Partial Sends

[src/hooks.c](../src/hooks.c) - `hook_send()`

**Problem:** Game assumes `send()` always sends all data in one call

**Solution:**
```c
int WSAAPI hook_send(SOCKET s, const char *buf, int len, int flags)
{
    int total_sent = 0;
    int retry_count = 0;

    while (total_sent < len && retry_count < SEND_MAX_RETRIES)
    {
        int result = real_send(s, buf + total_sent, len - total_sent, flags);

        if (result == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
            {
                // Buffer full, wait and retry
                Sleep(SEND_RETRY_DELAY_MS);
                pump_pending_messages();  // keep the loading dialog responsive
                retry_count++;
                continue;
            }
            return SOCKET_ERROR;  // Real error
        }

        total_sent += result;
    }

    return total_sent;
}
```

**Configuration:**
- `SEND_MAX_RETRIES` - Maximum retry attempts (default: `5000` ≈5 s)
- `SEND_RETRY_DELAY_MS` - Delay between retries (default: 1 ms)

**Impact:** Handles network buffer congestion gracefully with automatic retries.

### Server Function Hook - Packet Validation Fix

[src/hooks.c](../src/hooks.c) - `hook_srv_gameStreamReader()`

**Problem:** server.dll sets persistent error flags on packet validation failures, causing permanent "Out of Sync" state

**Solution:**
```c
int __cdecl hook_srv_gameStreamReader(int *ctx, int received, int totalLen)
{
    // Call original packet validation function
    int result = real_srv_gameStreamReader(ctx, received, totalLen);

    // Reset persistent error state
    if (ctx[0xE] < 0)
    {
        ctx[0xE] = 0;  // Clear error flag at offset 0xE
    }

    // Convert failures to success for retry
    if (result < 0)
    {
        result = 0;  // Allow higher-level code to retry
    }

    return result;
}
```

**Context Structure:**
- `ctx[0xE]` - Error state field (values: -3=CRC error, -1=stream error, 0=ok, 1=success)
- `ctx[0x13]` - Expected checksum
- `ctx[0x17]` - Packet signature ('r')

**Impact:** Prevents permanent desync by resetting error states, allowing natural retry mechanisms.

## Version Detection

### Multi-Version Support

Different game editions have the packet validation function at different offsets:

| Version | SHA256 | RVA Offset |
|---------|--------|------------|
| Steam (German) | `b341730...` | 0x3720 |
| GOG | `3cc2ce9...` | 0x3960 |
| Unknown | - | Pattern search |

### Detection Strategy

[src/hooks.c](../src/hooks.c) - `detect_server_version()` + `src/pattern_matcher.c`

1. **Pattern matching first** - `find_srv_gameStreamReader_by_pattern()` scans for `PUSH ECX … 0F 84/85` with wildcards, validates `PUSH ECX` + bounded `JE`/`JNE` targets
2. **SHA256 fallback** - `calculate_file_sha256()` (CryptoAPI `PROV_RSA_AES` → `PROV_RSA_FULL`) vs `known_versions[]` in `src/versions.c`
3. **Fail closed** - returns `0` on miss (no default RVA); `init_server_module()` aborts and logs

**Pattern Search:**
```c
// rizin-verified: 0x51 PUSH ECX … 0F 84/85 wildcards … 8B 45 38
// src/pattern_matcher.c:SRV_GAMESTREAMREADER_PATTERN (36 B) + MASK
// find_first_valid_match(): scan every occurrence in address order and
// accept the first whose JE/JNE targets stay inside the module
PATTERN_MATCH_RESULT r = find_first_valid_match(base, size, &rva);
```

**SHA256 Verification:**
```c
// src/sha256.c: CryptAcquireContext(PROV_RSA_AES||PROV_RSA_FULL) + CryptCreateHash(CALG_SHA_256)
char hex[65]; calculate_file_sha256(wpath, hex, sizeof(hex));
for (i in known_versions) if (strcmp(hex, known_versions[i].sha256_hash)==0) return target_rva;
```

### Caller Detection

Hooks only apply to calls from `server.dll` (cached range, not `GetModuleHandleEx` per call).

[src/hooks.c](../src/hooks.c) - `is_caller_from_server()` + `init_server_module()`

```c
// cached at init_server_module(): g_server_base/size from GetModuleInformation(g_hServerDll)
BOOL is_caller_from_server(uintptr_t a){
    if (g_server_base==0 || g_server_size==0) return FALSE;
    return a >= g_server_base && a < g_server_base + g_server_size;
}
```

**Usage:**
```c
// In hook function, get the return address. CALLER_IP() wraps
// __builtin_return_address(0); the build is clang-based (zig cc) and
// errors out on any other compiler.
void *caller = CALLER_IP();

// Only apply special logic for server.dll calls
if (is_caller_from_server((uintptr_t)caller))
{
    // Apply network fix
}
```

## Error Handling Strategy

### Layered Error Handling

1. **Windows API level** - Handle `WSAEWOULDBLOCK`, partial sends
2. **Game library level** - Reset server.dll error states
3. **Application level** - Game's existing error handling (unchanged)

### Logging Levels

- `[HOOK]` - Initialization and hook creation status
- `[WS2 HOOK]` - Winsock function interception events
- `[SERVER HOOK]` - Server.dll function hook events
- `[CONFIG]` - game.ini parsing
- `[PATTERN]` / `[SHA256]` - Version detection details
- `[NODELAY]` / `[FASTSYNC]` - Socket and pump-timing fixes
- `[TINY BUF]` / `[NET TRACE]` / `[EVT GUARD]` - Harness-only diagnostics

### Failure Policy: All-or-Nothing Hook Install

If any core hook fails to install (`srv_gameStreamReader`, `recv`,
`send`), `init_hooks()` tears down everything it created
(`MH_Uninitialize` + `reset_server_globals`) and reports failure. The game then
runs completely unpatched rather than with a partial fix, because a half-installed
set (for example send-retry without the recv `WSAEWOULDBLOCK` conversion) could
change protocol timing in ways the game never expects.

Best-effort extras are the exception and only log-and-skip on failure:
fastsync (Sleep IAT patch) and the harness evt guard never fail initialization.

## Performance Considerations

### Hook Overhead

Each hooked function adds minimal overhead:
- **Trampoline jump:** ~2-5 CPU cycles
- **Hook logic:** Varies by function (recv/send ~100-500 cycles with retries)
- **Logging:** Hot-path messages are rate-limited (once per 5 s per key), not removed

### Optimization Techniques

1. **Selective hooking** - Only hook specific callers (server.dll)
2. **Static variables** - Cache module addresses to avoid repeated lookups
3. **Fast path** - Immediate return for normal cases (no retry needed)
4. **Idempotent socket options** - `getsockopt` before `setsockopt` skips the
   syscall on already-configured sockets (see `maybe_set_nodelay`)

### Memory Footprint

- Plugin DLL: ~50-100 KB
- Hook trampolines: ~100 bytes per hook (3 core hooks, plus the harness evt guard when enabled)
- Runtime state: <1 KB
- Total overhead: <200 KB

### Network Performance Impact

Based on testing over 15+ in-game years:
- **Latency increase:** <1ms average (within normal variation)
- **Throughput impact:** Negligible (retries only on buffer full)
- **CPU usage:** <0.1% additional

The performance cost is far outweighed by the stability improvement - without the fix, the game is unplayable over VPN.

## Security Considerations

### Code Injection Safety

- **Read-only hooks:** Only intercepts calls, doesn't modify game code on disk
- **Process-local:** Only affects current game process
- **No elevation:** Doesn't require admin privileges
- **Unloadable:** Clean uninstall on process exit

### Attack Surface

The plugin:
- Does NOT open network listeners
- Does NOT load additional code from disk
- Does NOT modify other processes
- Does NOT persist beyond game session

### Antivirus Considerations

Some antivirus software may flag the plugin due to:
- Code injection techniques (hooking)
- DLL injection (ASI loader)
- Memory manipulation (MinHook)

This is a false positive - the code is open source and can be audited.

## Future Improvements

### Planned Features

1. **Configurable retry parameters** - Allow tuning via INI file
2. **Network statistics** - Track packet loss, retries, latency
3. **Hot-reload configuration** - Change settings without restart

### Extension Points

The architecture supports adding:
- Additional hooks for other functions
- Custom network protocols
- Packet inspection/logging
- Network simulation for testing

## Harness (xdotool + Lua)

`harness/` is `gilde-net` + fully headless in-container weston + rootful Xwayland `:99 1152x864` (`cur_res=2`; RandR modes the game's init requires; llvmpipe by default, GPU via `docker-compose.gpu.yml`) + `ffmpeg` + lua-driven `drivers/{host,client,common}.sh` (`lua_do` in-process clicks; XTest does not reach submenu buttons) + `lua/`/`luaapi.asi` via `docker-compose.lua.yml` (`harness/LUA_INTEGRATION.md`). ASI loading is done by dxwrapper `LoadPlugins=1` (`d3d8=n,b` override); headless crash chain (`0x42980D`/`0x46B2CC`) root-caused and fixed, see `harness/README.md`.

## References

- [MinHook Documentation](https://github.com/TsudaKageyu/minhook)
- [Problem Analysis](problem-analysis.md)
- [Server DLL Versions](server-dll-versions.md)
- [Windows Winsock Documentation](https://docs.microsoft.com/en-us/windows/win32/winsock/)

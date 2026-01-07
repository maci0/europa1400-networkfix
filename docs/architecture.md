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

Europa 1400 Gold Edition includes an ASI plugin loader that automatically loads `.asi` files from the game directory at startup. This is the same mechanism used by many modding frameworks.

### Load Sequence

1. Game process starts (`Europa1400Gold_TL.exe`)
2. ASI loader discovers `networkfix.asi` in game directory
3. Windows loads the DLL into game process address space
4. `DllMain` is called with `DLL_PROCESS_ATTACH`
5. Plugin initialization begins

### DllMain Entry Point

[src/main.c:58-105](../src/main.c#L58-L105)

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
        // Cleanup on exit
        cleanup_hooks();
        close_logging();
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
    // 1. Initialize MinHook
    if (MH_Initialize() != MH_OK)
        return FALSE;

    // 2. Create hooks for Winsock functions
    create_hook("ws2_32.dll", "recv", hook_recv);
    create_hook("ws2_32.dll", "send", hook_send);

    // 3. Create hooks for timing functions
    create_hook("kernel32.dll", "GetTickCount", hook_GetTickCount);

    // 4. Detect and hook server.dll function
    detect_and_hook_server_function();

    // 5. Enable all hooks
    MH_EnableHook(MH_ALL_HOOKS);

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
- `hook_GetTickCount()` - Timing function hook
- `hook_srv_gameStreamReader()` - Server.dll packet validation hook
- `is_caller_from_server()` - Detects if caller is from server.dll

### 3. Logging Module ([src/logging.c](../src/logging.c), [src/logging.h](../src/logging.h))

**Responsibilities:**
- File-based logging to `hook_log.txt`
- Thread-safe log writing
- Initialization status reporting

**Key Functions:**
- `init_logging()` - Open log file
- `logf()` - Write formatted log message
- `close_logging()` - Close and flush log file

### 4. Pattern Matcher ([src/pattern_matcher.c](../src/pattern_matcher.c), [src/pattern_matcher.h](../src/pattern_matcher.h))

**Responsibilities:**
- Binary pattern search for unknown server.dll versions
- Instruction pattern matching for function signatures

**Key Functions:**
- `find_pattern_in_module()` - Search DLL for byte patterns

### 5. Version Detection ([src/versions.h](../src/versions.h), [src/sha256.c](../src/sha256.c))

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

    while (total_sent < len && retry_count < MAX_SEND_RETRIES)
    {
        int result = real_send(s, buf + total_sent, len - total_sent, flags);

        if (result == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
            {
                // Buffer full, wait and retry
                Sleep(SEND_RETRY_DELAY_MS);
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
- `MAX_SEND_RETRIES` - Maximum retry attempts (default: unlimited)
- `SEND_RETRY_DELAY_MS` - Delay between retries (default: 1ms)

**Impact:** Handles network buffer congestion gracefully with automatic retries.

### GetTickCount() Hook - Consistent Timing

[src/hooks.c](../src/hooks.c) - `hook_GetTickCount()`

**Problem:** Network latency variations cause timeout issues in game synchronization

**Solution:**
```c
DWORD WINAPI hook_GetTickCount(void)
{
    // Pass through to original function
    // Future: Could add time drift compensation if needed
    return real_GetTickCount();
}
```

**Current Status:** Passthrough implementation. Reserved for future timing adjustments if needed.

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

[src/hooks.c](../src/hooks.c) - `detect_server_version()`

1. **Pattern matching first** - Search for known instruction patterns
2. **SHA256 fallback** - If pattern fails, compute full module hash
3. **Default to Steam** - If all detection fails, use Steam offset

**Pattern Search:**
```c
// Search for distinctive instruction sequence
uint8_t pattern[] = {
    0x8B, 0x44, 0x24, 0x04,  // mov eax, [esp+4]
    0x8B, 0x48, 0x38,        // mov ecx, [eax+0x38]
    // ... more bytes
};

void *func = find_pattern_in_module(hServer, pattern, sizeof(pattern));
```

**SHA256 Verification:**
```c
// Hash entire server.dll
uint8_t hash[32];
sha256_compute_module(hServer, hash);

// Compare with known versions
if (memcmp(hash, STEAM_HASH, 32) == 0)
    return STEAM_RVA;
```

### Caller Detection

Hooks only apply to calls from server.dll, not other game code or system components.

[src/hooks.c](../src/hooks.c) - `is_caller_from_server()`

```c
BOOL is_caller_from_server(uintptr_t caller_addr)
{
    // Get server.dll address range
    static uintptr_t server_start = 0;
    static uintptr_t server_end = 0;

    if (!server_start)
    {
        HMODULE hServer = GetModuleHandle("server.dll");
        MODULEINFO mi;
        GetModuleInformation(GetCurrentProcess(), hServer, &mi, sizeof(mi));
        server_start = (uintptr_t)mi.lpBaseOfDll;
        server_end = server_start + mi.SizeOfImage;
    }

    // Check if caller address is within server.dll range
    return (caller_addr >= server_start && caller_addr < server_end);
}
```

**Usage:**
```c
// In hook function, get return address
void *return_addr = _ReturnAddress();

// Only apply special logic for server.dll calls
if (is_caller_from_server((uintptr_t)return_addr))
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
- `[CONFIG]` - Configuration parsing
- `[ERROR]` - Error conditions

### Graceful Degradation

If a hook fails to install:
- Log the failure
- Continue with other hooks
- Game may have reduced stability but won't crash

```c
if (!create_hook("ws2_32.dll", "recv", hook_recv))
{
    logf("[ERROR] Failed to hook recv, continuing anyway");
    // Game continues running
}
```

## Performance Considerations

### Hook Overhead

Each hooked function adds minimal overhead:
- **Trampoline jump:** ~2-5 CPU cycles
- **Hook logic:** Varies by function (recv/send ~100-500 cycles with retries)
- **Logging:** Disabled in release builds for hot paths

### Optimization Techniques

1. **Selective hooking** - Only hook specific callers (server.dll)
2. **Static variables** - Cache module addresses to avoid repeated lookups
3. **Fast path** - Immediate return for normal cases (no retry needed)
4. **Thread-local storage** - Avoid lock contention in hot paths

### Memory Footprint

- Plugin DLL: ~50-100 KB
- Hook trampolines: ~100 bytes per hook × 4 hooks = ~400 bytes
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

1. **Automatic version detection** - Remove need for known SHA256 hashes
2. **Configurable retry parameters** - Allow tuning via INI file
3. **Network statistics** - Track packet loss, retries, latency
4. **Hot-reload configuration** - Change settings without restart

### Extension Points

The architecture supports adding:
- Additional hooks for other functions
- Custom network protocols
- Packet inspection/logging
- Network simulation for testing

## References

- [MinHook Documentation](https://github.com/TsudaKageyu/minhook)
- [Problem Analysis](problem-analysis.md)
- [Server DLL Versions](server-dll-versions.md)
- [Windows Winsock Documentation](https://docs.microsoft.com/en-us/windows/win32/winsock/)

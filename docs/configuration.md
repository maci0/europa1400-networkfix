# Configuration Guide

This document describes the configuration options and runtime behavior customization for the europa1400-networkfix plugin.

## Table of Contents

- [Overview](#overview)
- [game.ini Configuration](#gameini-configuration)
- [Build-time Configuration](#build-time-configuration)
- [Logging Configuration](#logging-configuration)
- [Network Behavior Tuning](#network-behavior-tuning)
- [Advanced Configuration](#advanced-configuration)
- [Environment Variables](#environment-variables)

## Overview

The europa1400-networkfix plugin is designed to work out-of-the-box with minimal configuration. Most settings are automatically detected or use sensible defaults. However, there are several configuration options available for advanced users and specific scenarios.

### Configuration Hierarchy

1. **Build-time constants** - Compiled into the DLL (requires rebuild)
2. **game.ini settings** - Read from Europa 1400's configuration file
3. **Runtime detection** - Automatic version/path detection
4. **Hardcoded defaults** - Fallback values

## game.ini Configuration

The plugin reads the Europa 1400 `game.ini` file to locate `server.dll`. This is the same configuration file used by the game itself.

### Location

The `game.ini` file is located in the game's root directory:
- Steam: `C:\Program Files (x86)\Steam\steamapps\common\Europa 1400\game.ini`
- GOG: `C:\GOG Games\Europa 1400\game.ini`

### Server Path Configuration

The plugin looks for the `[Network]` section to find the server.dll path:

```ini
[Network]
ServerPath=Server\server.dll
```

**Default behavior:**
- If `game.ini` is found, the `ServerPath` is read from `[Network]` section
- If not found or invalid, falls back to `Server\server.dll`
- Path is relative to game executable directory

### Example game.ini

```ini
[Game]
Language=German
Version=1.0.0.0

[Network]
ServerPath=Server\server.dll
Port=2303

[Graphics]
Resolution=1920x1080
Fullscreen=1
```

**Note:** Only the `ServerPath` under `[Network]` is used by the plugin. Other settings are for the game itself.

### Custom Server.dll Location

To use a server.dll from a custom location:

1. Open `game.ini` in a text editor
2. Modify the `ServerPath` under `[Network]`:
   ```ini
   [Network]
   ServerPath=CustomPath\server.dll
   ```
3. Save and restart the game

**Path rules:**
- Relative paths are relative to game executable directory
- Absolute paths are supported (e.g., `C:\Custom\server.dll`)
- Backslashes (`\`) and forward slashes (`/`) both work
- Paths with spaces require no special quoting

## Build-time Configuration

These constants are defined in source files and require recompilation to change.

### Network Retry Settings

Defined in [src/hooks.c:38-39](../src/hooks.c#L38-L39):

```c
#define SEND_MAX_RETRIES INT_MAX    // Maximum retry attempts
#define SEND_RETRY_DELAY_MS 1       // Delay between retries in milliseconds
```

**SEND_MAX_RETRIES:**
- Controls how many times `send()` retries on `WSAEWOULDBLOCK`
- Default: `INT_MAX` (effectively unlimited retries)
- Recommended values:
  - `INT_MAX` - Never give up (default, safest)
  - `100` - Up to 100 retries (~100ms total)
  - `1000` - Up to 1000 retries (~1s total)
  - `10` - Give up quickly for debugging

**SEND_RETRY_DELAY_MS:**
- Delay between send retry attempts
- Default: `1` ms (matches original implementation)
- Recommended values:
  - `1` - Fast retries, matches original (default)
  - `5` - Slower retries, less CPU usage
  - `10` - Conservative, good for slow networks
  - `0` - No delay, maximum retry rate

**To change:**
1. Edit [src/hooks.c](../src/hooks.c)
2. Modify the `#define` values
3. Rebuild: `make clean && make`

**Example - Faster retries with limit:**
```c
#define SEND_MAX_RETRIES 500         // Max 500 retries
#define SEND_RETRY_DELAY_MS 0        // No delay
```

**Example - Conservative retries:**
```c
#define SEND_MAX_RETRIES 100         // Max 100 retries
#define SEND_RETRY_DELAY_MS 10       // 10ms delay
```

### Default Server Path

Defined in [src/hooks.c:37](../src/hooks.c#L37):

```c
#define DEFAULT_SERVER_PATH "Server\\server.dll"
```

**Purpose:** Fallback path if `game.ini` cannot be read.

**To change:**
1. Edit [src/hooks.c](../src/hooks.c)
2. Modify `DEFAULT_SERVER_PATH`
3. Rebuild: `make clean && make`

## Logging Configuration

### Log File Location

The plugin writes logs to `hook_log.txt` in the game directory:
```
C:\Program Files (x86)\Steam\steamapps\common\Europa 1400\hook_log.txt
```

**Cannot be changed** without modifying source code.

### Log Rotation

Defined in [src/logging.c:20](../src/logging.c#L20):

```c
static const uint32_t MAX_LOG_LINES = 50000u;
```

**Behavior:**
- When log exceeds 50,000 lines, it automatically truncates
- Prevents log file from growing indefinitely
- No old logs are preserved (entire file is reset)

**To change:**
1. Edit [src/logging.c](../src/logging.c)
2. Modify `MAX_LOG_LINES`
3. Rebuild: `make debug` (logging most relevant in debug builds)

**Example - Smaller log:**
```c
static const uint32_t MAX_LOG_LINES = 10000u;  // Only 10k lines
```

### Log Buffer Size

Defined in [src/logging.c:21](../src/logging.c#L21):

```c
static const size_t LOG_BUFFER_SIZE = 2048;
```

**Purpose:** Maximum length of a single log message.

**To change:** Edit value and rebuild (rarely needed).

### Log Level Control

Currently, there are no runtime log levels. All log messages are written.

**To reduce verbosity:**
1. Use release build: `make` (less verbose than `make debug`)
2. Comment out specific `logf()` calls in source code
3. Rebuild

**Categories in logs:**
- `[HOOK]` - Initialization and lifecycle
- `[WS2 HOOK]` - Winsock function calls
- `[SERVER HOOK]` - Server.dll function calls
- `[PATTERN]` - Pattern matching details
- `[ERROR]` - Error conditions

### Rate-Limited Logging

Some log messages use rate limiting to prevent spam:

Defined in [src/logging.h:18](../src/logging.h#L18):

```c
#define LOG_RATE_LIMIT_MS 1000
```

**Purpose:** Minimum time (ms) between repeated log messages.

**Example usage in code:**
```c
logf_rate_limited("recv_error", "[WS2 HOOK] recv error: %d", error);
```

Only logs once per second even if called thousands of times.

## Network Behavior Tuning

### Receive Timeout Behavior

The `recv()` hook converts `WSAEWOULDBLOCK` errors to graceful returns:

```c
// In hook_recv()
if (error == WSAEWOULDBLOCK)
{
    WSASetLastError(0);
    return 0;  // No data available
}
```

**Cannot be configured** - this is the core fix.

### Send Retry Logic

The `send()` hook retries on buffer full conditions:

```c
// Simplified logic
while (total_sent < len && retry_count < MAX_SEND_RETRIES)
{
    result = real_send(...);
    if (result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
    {
        Sleep(SEND_RETRY_DELAY_MS);
        retry_count++;
        continue;
    }
    total_sent += result;
}
```

**Configurable via:** `SEND_MAX_RETRIES` and `SEND_RETRY_DELAY_MS` (see above).

### Server Error State Reset

The server.dll hook resets persistent error states:

```c
// In hook_srv_gameStreamReader()
if (ctx[0xE] < 0)
{
    ctx[0xE] = 0;  // Clear error flag
}
if (result < 0)
{
    result = 0;    // Convert failure to success
}
```

**Cannot be configured** - this is the core fix.

## Advanced Configuration

### Selective Hooking

By default, hooks apply selectively to `server.dll` callers only.

**Caller detection logic:**

```c
BOOL is_caller_from_server(uintptr_t caller_addr)
{
    // Check if return address is within server.dll range
    return (caller_addr >= server_start && caller_addr < server_end);
}
```

**To disable selective hooking:**
1. Edit hook functions to always apply fixes
2. Remove `is_caller_from_server()` checks
3. Rebuild

**Warning:** Disabling selective hooking affects all network operations system-wide, not just the game.

### Version Detection Priority

The plugin tries multiple methods to find the server function:

1. **Pattern matching** - Search for instruction patterns
2. **SHA256 lookup** - Match against known versions
3. **Default fallback** - Use Steam version offset (0x3720)

**To force a specific RVA:**

Edit [src/hooks.c](../src/hooks.c) in `detect_server_version()`:

```c
static DWORD detect_server_version()
{
    // Force specific RVA (skip detection)
    logf("[HOOK] Using forced RVA 0x3720");
    return 0x3720;  // Steam version

    // Original detection code below...
}
```

**Use cases:**
- Testing specific version
- Pattern matching fails but you know correct offset
- Debugging version detection issues

### Adding Custom Versions

To add support for a new game version:

1. **Get server.dll SHA256:**
   ```bash
   sha256sum server.dll
   ```

2. **Find function RVA using Ghidra/IDA Pro**

3. **Add to [src/versions.h](../src/versions.h):**
   ```c
   // New version hash
   #define CUSTOM_VERSION_HASH "abcdef1234567890..."

   // New version RVA
   #define CUSTOM_VERSION_RVA 0x4000
   ```

4. **Register in known_versions array:**
   ```c
   static const struct {
       const char *version_name;
       const char *sha256_hash;
       DWORD target_rva;
   } known_versions[] = {
       {"Steam (German)", STEAM_VERSION_HASH, STEAM_VERSION_RVA},
       {"GOG", GOG_VERSION_HASH, GOG_VERSION_RVA},
       {"Custom", CUSTOM_VERSION_HASH, CUSTOM_VERSION_RVA},  // Add here
       {NULL, NULL, 0}
   };
   ```

5. **Rebuild and test:**
   ```bash
   make clean && make debug
   ```

See [server-dll-versions.md](server-dll-versions.md) for detailed version documentation.

## Environment Variables

The plugin does not currently use environment variables for configuration.

**Possible future enhancement:**
```
NETWORKFIX_SERVER_PATH=C:\Custom\server.dll
NETWORKFIX_LOG_LEVEL=DEBUG
NETWORKFIX_MAX_RETRIES=100
```

These would override default settings without requiring rebuild.

## Configuration Examples

### Example 1: Conservative Retry Policy

For very stable networks where retries indicate real problems:

**Edit [src/hooks.c](../src/hooks.c):**
```c
#define SEND_MAX_RETRIES 10          // Only try 10 times
#define SEND_RETRY_DELAY_MS 100      // Wait 100ms between tries
```

**Build:**
```bash
make clean && make
```

**Result:** Plugin gives up faster on persistent errors.

### Example 2: Aggressive Retry Policy

For very unstable networks (poor WiFi, high latency VPN):

**Edit [src/hooks.c](../src/hooks.c):**
```c
#define SEND_MAX_RETRIES INT_MAX     // Never give up
#define SEND_RETRY_DELAY_MS 5        // Wait 5ms between tries
```

**Build:**
```bash
make clean && make
```

**Result:** Plugin keeps trying much longer.

### Example 3: Custom Server Path

If your server.dll is in a non-standard location:

**Edit game.ini:**
```ini
[Network]
ServerPath=Mods\NetworkFix\server.dll
```

**Or use absolute path:**
```ini
[Network]
ServerPath=C:\Europa1400Custom\server.dll
```

**Restart game** - no rebuild needed.

### Example 4: Minimal Logging

To reduce log file size:

**Edit [src/logging.c](../src/logging.c):**
```c
static const uint32_t MAX_LOG_LINES = 1000u;  // Smaller log
```

**Comment out verbose logging in [src/hooks.c](../src/hooks.c):**
```c
// logf("[WS2 HOOK] recv() called");  // Comment out
```

**Build:**
```bash
make clean && make
```

### Example 5: Force GOG Version

If pattern matching fails on GOG version:

**Edit [src/hooks.c](../src/hooks.c) in `detect_server_version()`:**
```c
static DWORD detect_server_version()
{
    logf("[HOOK] Forcing GOG version RVA 0x3960");
    return 0x3960;  // GOG offset
}
```

**Build and test:**
```bash
make clean && make debug
```

## Troubleshooting Configuration Issues

### Problem: server.dll not found

**Check:**
1. Is `game.ini` in game directory?
2. Does `[Network]` section exist?
3. Is `ServerPath` correct?
4. Check `hook_log.txt` for exact error

**Solution:**
- Verify path in `game.ini`
- Use absolute path if relative doesn't work
- Check Windows file permissions

### Problem: Wrong function hooked

**Symptoms:** Crashes, wrong behavior, log shows unexpected RVA

**Solution:**
1. Check `hook_log.txt` for detected version
2. Calculate your server.dll SHA256
3. Compare against known versions
4. If unknown, use pattern matching or add custom version

### Problem: Too many retries causing lag

**Symptoms:** Game freezes briefly during network operations

**Solution:**
- Reduce `SEND_MAX_RETRIES` to 100 or less
- Increase `SEND_RETRY_DELAY_MS` to reduce CPU usage
- Check network quality (may need VPN configuration)

### Problem: Not enough retries, still desyncing

**Symptoms:** Still getting "Out of Sync" errors

**Solution:**
- Increase `SEND_MAX_RETRIES` to `INT_MAX`
- Reduce `SEND_RETRY_DELAY_MS` to 1 or 0
- Verify hooks are actually being called (check logs)
- Test with debug build for verbose logging

### Problem: Log file grows too large

**Solution:**
1. Reduce `MAX_LOG_LINES` in [src/logging.c](../src/logging.c)
2. Use release build instead of debug: `make` instead of `make debug`
3. Manually delete `hook_log.txt` periodically

## Configuration Best Practices

1. **Start with defaults** - They work for most scenarios
2. **Use debug build for testing** - More verbose logging helps diagnose issues
3. **Test thoroughly** - Play for multiple in-game years after configuration changes
4. **Document changes** - Keep notes on what you modified and why
5. **One change at a time** - Don't modify multiple settings simultaneously
6. **Back up working builds** - Save known-good .asi files
7. **Check logs first** - `hook_log.txt` contains valuable diagnostic information

## Future Configuration Improvements

Planned enhancements for easier configuration:

1. **INI-based configuration file** - Runtime settings without rebuild
2. **Configuration UI** - Simple tool to adjust settings
3. **Profile system** - Predefined configurations for different scenarios
4. **Hot-reload** - Apply configuration changes without restart
5. **Network statistics** - Real-time monitoring of retry behavior
6. **Auto-tuning** - Automatic adjustment based on network conditions

## See Also

- [Architecture Documentation](architecture.md) - Technical implementation details
- [Development Guide](development-guide.md) - Building and modifying the plugin
- [Problem Analysis](problem-analysis.md) - Why these fixes are necessary
- [Server DLL Versions](server-dll-versions.md) - Supported game versions

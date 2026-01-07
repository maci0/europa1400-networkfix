# Development Guide

This guide covers everything needed to build, modify, debug, and contribute to the europa1400-networkfix project.

## Table of Contents

- [Development Environment Setup](#development-environment-setup)
- [Building from Source](#building-from-source)
- [Project Structure](#project-structure)
- [Code Style Guidelines](#code-style-guidelines)
- [Debugging](#debugging)
- [Testing](#testing)
- [Adding New Hooks](#adding-new-hooks)
- [Version Detection](#version-detection)
- [Contributing](#contributing)
- [Release Process](#release-process)

## Development Environment Setup

### Windows Development

**Required Tools:**
- [Zig](https://ziglang.org/download/) 0.11.0 or later
- [Make for Windows](http://gnuwin32.sourceforge.net/packages/make.htm) or use Git Bash
- [clang-format](https://releases.llvm.org/download.html) (optional, for code formatting)

**Recommended IDEs:**
- Visual Studio Code with C/C++ extension
- Visual Studio 2019/2022 with C/C++ workload
- CLion

**Install Zig:**
```powershell
# Download from https://ziglang.org/download/
# Extract to C:\zig
# Add C:\zig to PATH
zig version  # Verify installation
```

### Linux Development (Cross-compilation)

**Required Tools:**
```bash
# Ubuntu/Debian
sudo apt-get install make clang-format git

# Arch Linux
sudo pacman -S make clang git

# Install Zig
curl -O https://ziglang.org/download/0.11.0/zig-linux-x86_64-0.11.0.tar.xz
tar -xf zig-linux-x86_64-0.11.0.tar.xz
sudo mv zig-linux-x86_64-0.11.0 /opt/zig
export PATH=$PATH:/opt/zig
```

**Testing on Linux:**
- Use Wine to run Europa 1400
- Plugin works identically under Wine
- Install target: `make install` copies to `~/.wine/drive_c/Guild`

### macOS Development (Cross-compilation)

**Required Tools:**
```bash
# Install Homebrew
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install dependencies
brew install make clang-format git

# Install Zig
brew install zig
```

## Building from Source

### Clone Repository

```bash
git clone --recursive https://github.com/maci0/europa1400-networkfix.git
cd europa1400-networkfix
```

**Important:** Use `--recursive` to fetch MinHook submodule. If you forgot:
```bash
git submodule update --init --recursive
```

### Build Commands

```bash
# Release build (optimized, no debug symbols)
make

# Debug build (symbols, verbose logging)
make debug

# Format source code
make format

# Clean build artifacts
make clean

# Install to Wine (Linux only)
make install
```

### Build Output

- **Release:** `bin/networkfix.asi` (~50 KB)
- **Debug:** `bin/networkfix-debug.asi` (~200 KB)

### Build Options

Edit [Makefile](../Makefile) to customize:

```makefile
# Change Zig compiler path
ZIG := /path/to/zig

# Add compiler flags
CFLAGS := -I$(MINHOOK_DIR)/include -Isrc -DCUSTOM_FLAG

# Add linker flags
LDFLAGS := -lc -lws2_32 -lshlwapi -ladvapi32 -lcustomlib

# Change optimization level
# -O ReleaseSmall (default) - Smallest size
# -O ReleaseFast - Maximum speed
# -O ReleaseSafe - Safety checks enabled
# -O Debug - No optimization, full debug info
```

### Build Troubleshooting

**Problem:** `zig: command not found`
- **Solution:** Ensure Zig is in PATH: `export PATH=$PATH:/path/to/zig`

**Problem:** `git submodule: No such file`
- **Solution:** Initialize submodules: `git submodule update --init --recursive`

**Problem:** MinHook compilation errors
- **Solution:** Ensure using Zig 0.11.0+, check submodule is at correct commit

**Problem:** `make: command not found` on Windows
- **Solution:** Use Git Bash or install Make for Windows

## Project Structure

```
europa1400-networkfix/
├── src/                        # Source code
│   ├── main.c                  # DLL entry point and initialization
│   ├── hooks.c/h               # Hook implementations
│   ├── logging.c/h             # Logging system
│   ├── pattern_matcher.c/h    # Binary pattern search
│   ├── sha256.c/h              # SHA256 hashing for version detection
│   └── versions.h              # Known server.dll versions
├── docs/                       # Documentation
│   ├── architecture.md         # Technical architecture
│   ├── problem-analysis.md     # Problem analysis
│   ├── server-dll-versions.md  # Version documentation
│   └── development-guide.md    # This file
├── vendor/                     # Third-party dependencies
│   └── minhook/                # MinHook hooking library
├── bin/                        # Build output (created by make)
├── Makefile                    # Build configuration
├── .clang-format               # Code formatting rules
├── LICENSE                     # GPLv3 license
└── README.md                   # User documentation
```

### Key Files

- [src/main.c](../src/main.c) - Entry point, initialization thread
- [src/hooks.c](../src/hooks.c) - All hook implementations
- [src/hooks.h](../src/hooks.h) - Hook interface definitions
- [src/logging.c](../src/logging.c) - Thread-safe file logging
- [src/pattern_matcher.c](../src/pattern_matcher.c) - Function pattern search
- [src/versions.h](../src/versions.h) - Known server.dll SHA256 hashes
- [Makefile](../Makefile) - Build system

## Code Style Guidelines

### Formatting

Use clang-format for consistent formatting:
```bash
make format  # Format all source files
```

See [.clang-format](../.clang-format) for rules:
- **Indent:** 4 spaces (no tabs)
- **Braces:** Allman style (braces on new line)
- **Line length:** 120 characters max
- **Pointer alignment:** Right-aligned (`char *ptr`)

### Naming Conventions

**Functions:**
```c
// Public API (hooks.h)
BOOL init_hooks(void);
void cleanup_hooks(void);

// Hook implementations
int WSAAPI hook_recv(SOCKET s, char *buf, int len, int flags);

// Private/static helpers
static BOOL create_hook_internal(const char *name);
```

**Variables:**
```c
// Global variables (g_ prefix)
HMODULE g_hModule = NULL;

// Static variables (s_ prefix)
static int s_retry_count = 0;

// Local variables (snake_case)
int total_sent = 0;
DWORD error_code = GetLastError();
```

**Constants:**
```c
// Preprocessor defines (UPPER_CASE)
#define MAX_SEND_RETRIES 100
#define SEND_RETRY_DELAY_MS 1

// Const variables (snake_case)
static const char *SERVER_DLL_NAME = "server.dll";
```

**Types:**
```c
// Structs (PascalCase)
typedef struct ServerContext {
    int error_state;
    uint32_t checksum;
} ServerContext;

// Function pointers (PascalCase with _t suffix)
typedef int (WSAAPI *RecvFunc_t)(SOCKET, char*, int, int);
```

### Comments

**File headers:**
```c
/*
 * hooks.c: Network and game function hooks for Europa 1400 network fix.
 *
 * This file implements the core hooking logic that intercepts Windows
 * networking APIs and server.dll functions to add retry and error handling.
 */
```

**Function documentation:**
```c
/**
 * Creates and enables a hook for the specified function.
 *
 * Uses MinHook to create an inline hook that redirects calls to the target
 * function through our hook handler. The original function remains callable
 * via the trampoline pointer.
 *
 * @param module_name Name of DLL containing target function (e.g., "ws2_32.dll")
 * @param func_name Name of function to hook (e.g., "recv")
 * @param hook_func Pointer to our hook handler function
 * @return TRUE if hook created successfully, FALSE otherwise
 */
static BOOL create_hook(const char *module_name, const char *func_name, void *hook_func)
{
    // Implementation
}
```

**Inline comments:**
```c
// Convert WSAEWOULDBLOCK to graceful "no data available" return
if (error == WSAEWOULDBLOCK)
{
    WSASetLastError(0);
    return 0;  // Try again later
}

// Reset persistent error state at context offset 0xE
if (ctx[0xE] < 0)
{
    ctx[0xE] = 0;  // Clear -1/-3 error flags
}
```

### Error Handling

**Always check return values:**
```c
if (MH_Initialize() != MH_OK)
{
    logf("[ERROR] MinHook initialization failed");
    return FALSE;
}

HMODULE hServer = GetModuleHandle("server.dll");
if (!hServer)
{
    logf("[ERROR] Failed to load server.dll");
    return FALSE;
}
```

**Use logging for errors:**
```c
// Log initialization failures
if (!init_logging(hModule))
{
    OutputDebugStringA("[HOOK] Failed to initialize logging");
    return FALSE;
}

// Log hook creation failures (but continue)
if (!create_hook("ws2_32.dll", "recv", hook_recv))
{
    logf("[ERROR] Failed to hook recv, continuing anyway");
}
```

## Debugging

### Debug Build

Build with debug symbols and verbose logging:
```bash
make debug
```

This enables:
- Full debug symbols for debuggers
- Verbose logging to `hook_log.txt`
- No optimization (easier to step through)

### Using a Debugger

**Visual Studio:**
1. Attach to running Europa 1400 process
2. Load symbols from `networkfix-debug.asi`
3. Set breakpoints in hook functions
4. Trigger by performing network operations in game

**WinDbg:**
```
# Attach to process
windbg -p <Europa1400_PID>

# Load symbols
.reload /f networkfix-debug.asi

# Set breakpoint
bp hook_recv

# Continue execution
g
```

**GDB (Linux/Wine):**
```bash
# Run game under debugger
gdb --args wine Europa1400Gold_TL.exe

# Set breakpoint
break hook_recv

# Run
run
```

### Logging

All log output goes to `hook_log.txt` in game directory.

**Log categories:**
- `[HOOK]` - Hook initialization and status
- `[WS2 HOOK]` - Winsock recv/send calls
- `[SERVER HOOK]` - Server.dll function calls
- `[CONFIG]` - Configuration file parsing
- `[ERROR]` - Error conditions

**Add logging:**
```c
#include "logging.h"

// Simple message
logf("[HOOK] Hook created successfully");

// Formatted output
logf("[WS2 HOOK] recv() returned %d bytes, socket %d", bytes, socket);

// Error with details
logf("[ERROR] Failed to create hook: %s (error %d)", func_name, GetLastError());
```

### Debugging Hook Issues

**Problem:** Hooks not being called
- Check `hook_log.txt` for "Hook created" messages
- Verify MinHook initialization succeeded
- Ensure function name spelling matches DLL export
- Check if antivirus is blocking

**Problem:** Crashes in hook function
- Build debug version with symbols
- Add logging at entry/exit of hook
- Check for null pointers
- Verify calling convention matches (WSAAPI, __cdecl, etc.)

**Problem:** Game doesn't load plugin
- Verify `.asi` file is in game directory
- Check for `DllMain` entry point
- Ensure targeting 32-bit Windows (x86)
- Check Windows Event Viewer for load errors

### Memory Debugging

**Check for leaks:**
```c
// Add to DLL_PROCESS_DETACH
case DLL_PROCESS_DETACH:
    logf("[HOOK] Cleaning up hooks");
    cleanup_hooks();  // Must free all allocated memory
    close_logging();
    break;
```

**Use Windows debugging tools:**
- Process Explorer - View loaded DLLs
- VMMap - View memory allocations
- DebugView - Capture OutputDebugString messages

## Testing

### Manual Testing

1. **Build plugin:**
   ```bash
   make debug  # Use debug for verbose logging
   ```

2. **Install:**
   - Copy `bin/networkfix-debug.asi` to game directory
   - Rename to `networkfix.asi`

3. **Test scenarios:**
   - Start multiplayer game over VPN
   - Simulate packet loss (use network emulation tools)
   - Test with different game versions (Steam, GOG)
   - Verify `hook_log.txt` shows successful initialization

4. **Verify fixes:**
   - Play for extended period (multiple in-game years)
   - Monitor for "Out of Sync" errors (should not occur)
   - Check log for retry events

### Automated Testing

Currently no automated tests. Future improvements:
- Unit tests for pattern matching
- Mock Winsock functions for hook testing
- Automated game session testing

### Testing on Different Versions

**Steam version:**
- SHA256: `b341730ba273255fb0099975f30a7b1a950e322be3a491bfd8e137781ac97f06`
- Expected RVA: 0x3720

**GOG version:**
- SHA256: `3cc2ce9049e41ab6d0eea042df4966fbf57e5e27c67fb923e81709d2683609d1`
- Expected RVA: 0x3960

**Test checklist:**
- [ ] Hook initialization succeeds
- [ ] recv/send hooks intercept calls
- [ ] Server.dll function detected and hooked
- [ ] Multiplayer game completes without desync
- [ ] Log shows expected hook calls
- [ ] No crashes or errors

### Performance Testing

**Measure overhead:**
```c
// Add to hook functions
static uint64_t total_calls = 0;
static uint64_t total_cycles = 0;

uint64_t start = __rdtsc();
// Hook logic here
uint64_t end = __rdtsc();

total_cycles += (end - start);
total_calls++;

// Log periodically
if (total_calls % 10000 == 0)
{
    logf("[PERF] Avg cycles per call: %llu", total_cycles / total_calls);
}
```

**Monitor game performance:**
- FPS should remain unchanged
- Network latency increase should be <1ms
- CPU usage should be negligible

## Adding New Hooks

### Step-by-Step Process

**1. Identify target function:**
```c
// Example: Hook closesocket to track connection cleanup
int WSAAPI closesocket(SOCKET s);
```

**2. Add function pointer type:**
```c
// In hooks.c
typedef int (WSAAPI *ClosesocketFunc_t)(SOCKET s);
static ClosesocketFunc_t real_closesocket = NULL;
```

**3. Implement hook function:**
```c
int WSAAPI hook_closesocket(SOCKET s)
{
    // Log the call
    logf("[WS2 HOOK] closesocket(%d)", s);

    // Add custom logic here
    // ...

    // Call original function
    return real_closesocket(s);
}
```

**4. Create hook in init_hooks():**
```c
BOOL init_hooks(void)
{
    // ... existing hooks ...

    // Create closesocket hook
    if (!create_hook("ws2_32.dll", "closesocket", hook_closesocket, (void**)&real_closesocket))
    {
        logf("[ERROR] Failed to hook closesocket");
        // Continue anyway
    }

    // ... rest of initialization ...
}
```

**5. Add function prototype to hooks.h:**
```c
// In hooks.h
int WSAAPI hook_closesocket(SOCKET s);
```

**6. Test:**
- Build: `make debug`
- Install and run game
- Check `hook_log.txt` for closesocket calls
- Verify no crashes or unexpected behavior

### Hook Best Practices

**Do:**
- Always call original function unless intentionally blocking
- Log important events for debugging
- Check return values and error codes
- Handle all error paths gracefully
- Use proper calling convention (WSAAPI, __cdecl, etc.)

**Don't:**
- Assume parameters are valid (null checks!)
- Hold locks during original function call (deadlock risk)
- Throw exceptions (no C++ exception handling)
- Perform expensive operations in hot paths
- Modify read-only memory

### Selective Hooking

Only apply hooks to specific callers:
```c
int WSAAPI hook_recv(SOCKET s, char *buf, int len, int flags)
{
    // Get caller address
    void *caller = _ReturnAddress();

    // Only apply fix for server.dll calls
    if (is_caller_from_server((uintptr_t)caller))
    {
        // Custom logic for server.dll
        logf("[SERVER HOOK] recv() called from server.dll");
    }

    // Call original function
    return real_recv(s, buf, len, flags);
}
```

## Version Detection

### Adding a New Game Version

**1. Obtain server.dll from new version**

**2. Calculate SHA256 hash:**
```bash
# Linux/macOS
sha256sum server.dll

# Windows (PowerShell)
Get-FileHash server.dll -Algorithm SHA256
```

**3. Find packet validation function:**
- Use Ghidra or IDA Pro to analyze server.dll
- Look for function with error state checking pattern
- Note the RVA (Relative Virtual Address)

**4. Add to versions.h:**
```c
// In src/versions.h
#define NEWVERSION_HASH \
    {0xaa, 0xbb, 0xcc, ...}  // SHA256 bytes

#define NEWVERSION_RVA 0x1234  // Function offset
```

**5. Update version detection:**
```c
// In src/hooks.c - detect_server_version()
static const uint8_t newversion_hash[] = NEWVERSION_HASH;
if (memcmp(hash, newversion_hash, 32) == 0)
{
    logf("[HOOK] Detected NewVersion server.dll");
    return NEWVERSION_RVA;
}
```

**6. Document in server-dll-versions.md:**
- Add version details
- Include SHA256 hash
- Document RVA offset
- Note any behavioral differences

### Pattern-Based Detection

Alternative to hardcoded hashes:
```c
// Search for instruction pattern
uint8_t pattern[] = {
    0x8B, 0x44, 0x24, 0x04,  // mov eax, [esp+4]
    0x8B, 0x48, 0x38,        // mov ecx, [eax+0x38]
    0x83, 0xF9, 0xFD,        // cmp ecx, -3
    // ... more bytes
};

void *func = find_pattern_in_module(hServer, pattern, sizeof(pattern));
if (func)
{
    DWORD rva = (DWORD)((uintptr_t)func - (uintptr_t)hServer);
    logf("[HOOK] Found function via pattern at RVA 0x%X", rva);
    return rva;
}
```

## Contributing

### Contribution Guidelines

**We welcome contributions!** Please follow these guidelines:

1. **Open an issue first** for major changes
2. **Follow code style** (use `make format`)
3. **Test thoroughly** on at least one game version
4. **Update documentation** for new features
5. **Write clear commit messages**

### Commit Message Format

```
<type>: <short summary>

<detailed description>

<footer>
```

**Types:**
- `feat` - New feature
- `fix` - Bug fix
- `docs` - Documentation changes
- `refactor` - Code refactoring
- `test` - Adding tests
- `chore` - Build system, dependencies

**Examples:**
```
feat: add GOG version support

Implement version detection for GOG edition of Europa 1400. Adds
SHA256 hash check and RVA offset for packet validation function.

Tested with GOG version 1.0.0.0, multiplayer stable over 5 in-game years.
```

```
fix: memory leak in pattern matcher

Free allocated buffer in find_pattern_in_module() after search completes.
Leak occurred on every version detection attempt.

Fixes #123
```

### Pull Request Process

1. **Fork the repository**
2. **Create feature branch:** `git checkout -b feat/my-feature`
3. **Make changes and commit:** `git commit -m "feat: add feature"`
4. **Format code:** `make format`
5. **Test thoroughly** on Windows and/or Wine
6. **Push to fork:** `git push origin feat/my-feature`
7. **Open pull request** with description

**PR checklist:**
- [ ] Code follows style guidelines
- [ ] All source files formatted with clang-format
- [ ] Changes documented in code comments
- [ ] README.md updated if needed
- [ ] Tested on at least one game version
- [ ] No new compiler warnings
- [ ] Commit messages are clear

### Code Review

Maintainers will review for:
- Code quality and style
- Correctness and safety
- Performance impact
- Documentation completeness
- Test coverage

Please be patient - reviews may take a few days.

## Release Process

### Versioning

We use semantic versioning: `MAJOR.MINOR.PATCH`

- **MAJOR:** Breaking changes (rare)
- **MINOR:** New features, game version support
- **PATCH:** Bug fixes, documentation

### Creating a Release

**1. Update version references:**
- Update changelog
- Update README if needed

**2. Build release:**
```bash
make clean
make  # Release build
```

**3. Test release build:**
- Install on clean game installation
- Verify multiplayer functionality
- Check log output

**4. Create Git tag:**
```bash
git tag -a v1.2.0 -m "Release version 1.2.0"
git push origin v1.2.0
```

**5. GitHub Release:**
- Create release from tag
- Upload `bin/networkfix.asi`
- Write release notes
- Include SHA256 hash of .asi file

**6. Announce:**
- Update GitHub README
- Post to relevant communities
- Update documentation site

### Release Checklist

- [ ] Version numbers updated
- [ ] Clean build completed
- [ ] Release tested on Windows
- [ ] Release tested on Wine (Linux)
- [ ] Tested on all supported game versions
- [ ] Documentation updated
- [ ] Git tag created
- [ ] GitHub release created
- [ ] Binary SHA256 hash provided
- [ ] Release notes written
- [ ] Announcement posted

## Troubleshooting Development Issues

### Build fails with "undefined reference to MH_Initialize"
- **Cause:** MinHook submodule not initialized
- **Fix:** `git submodule update --init --recursive`

### Plugin doesn't load in game
- **Cause:** Missing DLL dependencies
- **Fix:** Check with Dependency Walker, ensure all DLLs present

### Hooks don't work under Wine
- **Cause:** Wine may have different Winsock implementation
- **Fix:** Test on actual Windows or newer Wine version

### Antivirus deletes plugin
- **Cause:** False positive from hooking behavior
- **Fix:** Add exception for game directory, submit false positive report

## Resources

- [MinHook Documentation](https://github.com/TsudaKageyu/minhook)
- [Zig Build System](https://ziglang.org/learn/build-system/)
- [Windows API Documentation](https://docs.microsoft.com/en-us/windows/win32/api/)
- [Ghidra Decompiler](https://ghidra-sre.org/)
- [x86 Disassembly](https://en.wikibooks.org/wiki/X86_Disassembly)

## Getting Help

- **Issues:** [GitHub Issues](https://github.com/maci0/europa1400-networkfix/issues)
- **Discussions:** [GitHub Discussions](https://github.com/maci0/europa1400-networkfix/discussions)
- **Email:** Check repository for maintainer contact

## License

This project is licensed under GPLv3. By contributing, you agree to license your contributions under the same license.

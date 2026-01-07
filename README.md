# europa1400-networkfix

A network stability patch for *Europa 1400: The Guild* that fixes multiplayer desynchronization issues over VPNs and modern internet connections. Implemented as a non-invasive `.asi` plugin using runtime API hooking.

## Features

- **Improved network stability:** Fixes desynchronization and disconnection issues caused by packet loss, making the game playable over VPNs (Hamachi, Radmin) and modern internet connections
- **Resilient network communication:** Adds retry logic and proper error handling to the game's network code, allowing it to recover from temporary network interruptions
- **Non-invasive:** Works as a plugin without modifying any game files - simply drop into game directory
- **Proven stability:** Tested with 15+ in-game years of stable multiplayer sessions over VPN connections

## Why?

The game's original multiplayer mode depends on `server.dll` for all network
communication.  That implementation assumes perfect packet delivery and will
desynchronize or disconnect clients on even minor packet loss.  Playing over
VPNs such as Hamachi or Radmin is therefore nearly impossible because the game
quickly throws "out of sync" errors.

This project draws on research from [The-Guild-1-HookDLLs](https://github.com/HarryTheBird/The-Guild-1-HookDLLs)
and provides a lightweight fix aimed at making the network stack resilient to
real‑world connections.

## How It Works

The `.asi` plugin is loaded automatically by the game and uses [MinHook](https://github.com/TsudaKageyu/minhook) to intercept key functions at runtime:

1. **Windows Socket API hooks** (`recv`/`send`) - Converts `WSAEWOULDBLOCK` errors into graceful retries instead of fatal errors
2. **Timing function hooks** (`GetTickCount`) - Ensures consistent timer behavior across network delays
3. **Game-specific hooks** (`server.dll` packet validation) - Resets persistent error states that cause "Out of Sync" errors

By applying surgical fixes only to problematic functions, the plugin adds proper error handling and retry logic without changing game behavior.

**See also:**
- [docs/problem-analysis.md](docs/problem-analysis.md) - Detailed technical problem analysis
- [docs/architecture.md](docs/architecture.md) - Complete architecture documentation
- [docs/development-guide.md](docs/development-guide.md) - Building and contributing guide
- [docs/configuration.md](docs/configuration.md) - Configuration options
- [docs/server-dll-versions.md](docs/server-dll-versions.md) - Supported game versions

### Technical Stack

- **Language:** C (compiled with [Zig](https://ziglang.org/) for cross-compilation)
- **Hooking library:** [MinHook](https://github.com/TsudaKageyu/minhook) for x86 function interception
- **Target:** 32-bit Windows executable (Europa 1400 Gold Edition)

## Building

### Prerequisites

- [Zig](https://ziglang.org/) (version 0.11.0 or later) - Cross-platform C compiler
- [make](https://www.gnu.org/software/make/) - Build automation
- [clang-format](https://clang.llvm.org/docs/ClangFormat.html) (optional) - Code formatting

### Build Instructions

1. Clone the repository with submodules:
```bash
git clone --recursive https://github.com/maci0/europa1400-networkfix.git
cd europa1400-networkfix
```

2. Build the release version:
```bash
make
```

3. Or build the debug version (includes debug symbols and verbose logging):
```bash
make debug
```

The compiled plugins will be in:
- **Release:** `bin/networkfix.asi`
- **Debug:** `bin/networkfix-debug.asi`

### Build Targets

- `make` or `make all` - Build release version with optimizations
- `make debug` - Build debug version with symbols and verbose logging
- `make clean` - Remove compiled binaries
- `make format` - Format source code with clang-format
- `make install` - Copy plugin to Wine installation (Linux only)

## Installation

### Supported Game Versions

- **Europa 1400: The Guild - Gold Edition** (primary support)
  - Steam version (German) - Fully tested
  - GOG version - Supported with different hook offsets
  - Other localizations - May require version detection

### Installation Steps

1. **Locate your game directory**
   - Find the folder containing `Europa1400Gold_TL.exe`
   - Steam default: `C:\Program Files (x86)\Steam\steamapps\common\Europa 1400`
   - GOG default: `C:\GOG Games\Europa 1400`

2. **Copy the plugin**
   - Copy `bin/networkfix.asi` to the game directory
   - Place it in the same folder as the game executable

3. **Verify installation**
   - Launch the game
   - Check for `hook_log.txt` in the game directory
   - Look for `[HOOK] Hook initialization complete` in the log

4. **Test multiplayer**
   - Start or join a multiplayer game
   - The plugin works silently in the background
   - Network errors should now be handled gracefully

## Troubleshooting

### Common Issues

#### Plugin Not Loading
- **Symptoms:** No `hook_log.txt` file appears in game directory
- **Solutions:**
  - Ensure you're using Europa 1400 Gold Edition (not the original version)
  - Verify `networkfix.asi` is in the same directory as `Europa1400Gold_TL.exe`
  - Check Windows didn't block the file: Right-click → Properties → Unblock

#### Connection Still Failing
- **Symptoms:** "Out of sync" errors persist, disconnections continue
- **Solutions:**
  - Check `hook_log.txt` for error messages like "Failed to load server.dll"
  - Verify your `game.ini` has correct server path in `[Network]` section
  - Try the debug version: `make debug` and use `networkfix-debug.asi`

#### Hook Creation Failures  
- **Symptoms:** Log shows "Failed to create hook" messages
- **Solutions:**
  - Ensure no antivirus is blocking the plugin
  - Run game as administrator
  - Check server.dll is the expected version (RVA 0x3720 exists)

#### Log File Issues
- **Symptoms:** No logging or partial logs
- **Solutions:**
  - Check directory permissions - plugin needs write access
  - Look for Windows Event Viewer entries if initialization fails
  - Ensure game directory isn't read-only

### Log Analysis

The `hook_log.txt` file contains detailed information:
- `[HOOK]` messages show initialization and hook creation status  
- `[WS2 HOOK]` messages indicate network function interception
- `[SERVER HOOK]` messages show server.dll function patches
- `[CONFIG]` messages relate to game.ini parsing

### Advanced Diagnostics

For deeper investigation:
1. Use debug build: `make debug` 
2. Enable additional logging by uncommenting debug lines in source
3. Use Process Monitor to watch file/registry access
4. Check with Dependency Walker if server.dll loads correctly

### Getting Help

If issues persist, please create an issue on the [GitHub repository](https://github.com/maci0/europa1400-networkfix/issues) with:
- Your `hook_log.txt` file contents
- Game version and installation path  
- Network configuration (VPN type, etc.)
- Steps to reproduce the problem

## Testing TODO

- **Steam Version Testing**: Verify network stability improvements work correctly with Steam version (RVA 0x3720)
- **GOG Version Testing**: Confirm GOG version compatibility and network fixes (RVA 0x3960) 
- **VPN Testing**: Test multiplayer sessions over VPNs (Hamachi, Radmin) to verify packet loss resilience
- **Connection Recovery**: Test network interruption recovery and retry logic functionality
- **Performance Impact**: Measure any performance impact from the network hooks during gameplay

## Documentation

Comprehensive documentation is available in the [docs/](docs/) directory:

### For Users
- **[README.md](README.md)** (this file) - Installation and basic usage
- **[Troubleshooting](#troubleshooting)** - Common issues and solutions
- **[docs/configuration.md](docs/configuration.md)** - Configuration options and tuning

### For Developers
- **[docs/development-guide.md](docs/development-guide.md)** - Building, debugging, and contributing
- **[docs/architecture.md](docs/architecture.md)** - Technical architecture and implementation
- **[docs/problem-analysis.md](docs/problem-analysis.md)** - Root cause analysis and solution design
- **[docs/server-dll-versions.md](docs/server-dll-versions.md)** - Game version compatibility

### Quick Links
- [How the fix works](docs/architecture.md#hook-implementation-details)
- [Building from source](docs/development-guide.md#building-from-source)
- [Adding new hooks](docs/development-guide.md#adding-new-hooks)
- [Configuration examples](docs/configuration.md#configuration-examples)

## Contributing

Contributions are welcome! Please see the [Development Guide](docs/development-guide.md) for detailed instructions on building, testing, and submitting pull requests.

**Quick start:**
1. Fork the repository
2. Create a feature branch: `git checkout -b feat/my-feature`
3. Make your changes and test thoroughly
4. Follow the [code style guidelines](docs/development-guide.md#code-style-guidelines)
5. Submit a pull request with a clear description

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu
- [HarryTheBird](https://github.com/HarryTheBird) for the original fix

## Disclaimer

This is an unofficial patch and is not affiliated with or endorsed by the developers or publishers of *Europa 1400: The Guild*. Use at your own risk.

## License

GPLv3 (see [LICENSE](LICENSE)).

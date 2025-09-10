# europa1400-networkfix

A network stability patch for *Europa 1400: The Guild* implemented as an `.asi` plugin.

## Features

- **Improved network stability:** Fixes desynchronization and disconnection issues caused by packet loss, making the game playable over VPNs and modern internet connections.
- **Resilient network communication:** Adds retry logic to the game's network code, allowing it to recover from temporary network interruptions.
- **Compatibility:** Works with the original game and does not modify any game files.

## Why?

The game's original multiplayer mode depends on `server.dll` for all network
communication.  That implementation assumes perfect packet delivery and will
desynchronize or disconnect clients on even minor packet loss.  Playing over
VPNs such as Hamachi or Radmin is therefore nearly impossible because the game
quickly throws "out of sync" errors.

This project draws on research from [The-Guild-1-HookDLLs](https://github.com/HarryTheBird/The-Guild-1-HookDLLs)
and provides a lightweight fix aimed at making the network stack resilient to
real‑world connections.

## How?

The `.asi` plugin is injected into the game and uses
[MinHook](https://github.com/TsudaKageyu/minhook) to intercept functions inside
`server.dll` and the Windows networking APIs.  By patching these routines we add
basic error handling and retry logic, allowing multiplayer sessions to survive
packet loss and latency.

The project builds a Windows dynamic library using [Zig](https://ziglang.org/)
and MinHook to implement the hooks.

## Building

### Prerequisites

- [Zig](https://ziglang.org/) (version 0.11.0 or later)
- [make](https://www.gnu.org/software/make/)

Ensure [Zig](https://ziglang.org/) is installed and initialize submodules:

```
git submodule update --init --recursive
make
```

The compiled plugin will be written to `bin/networkfix.asi`.

## Usage

This fix is for the Gold Edition of *Europa 1400: The Guild*.

1. **Copy the plugin.** Copy `bin/networkfix.asi` into the root directory of your *Europa 1400: The Guild - Gold Edition* installation. This is the directory that contains the game's main executable (`Europa1400Gold_TL.exe`).
2. **Run the game.** The plugin will be loaded automatically when you start the game.

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

## Contributing

Contributions are welcome! If you would like to contribute to this project, please fork the repository and submit a pull request.

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu
- [HarryTheBird](https://github.com/HarryTheBird) for the original fix

## Disclaimer

This is an unofficial patch and is not affiliated with or endorsed by the developers or publishers of *Europa 1400: The Guild*. Use at your own risk.

## License

GPLv3 (see [LICENSE](LICENSE)).

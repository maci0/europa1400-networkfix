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

This project draws on research from [The-Guild-1-HookDLLs](https://github.com/maci0/The-Guild-1-HookDLLs)
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

If you are experiencing issues, please check the `hook_log.txt` file in your game directory for any error messages. You can also create an issue on the [GitHub repository](https://github.com/maci0/europa1400-networkfix/issues).

## Contributing

Contributions are welcome! If you would like to contribute to this project, please fork the repository and submit a pull request.

## Credits

- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu
- [Zig](https://ziglang.org/)

## Disclaimer

This is an unofficial patch and is not affiliated with or endorsed by the developers or publishers of *Europa 1400: The Guild*. Use at your own risk.

## License

GPLv3 (see [LICENSE](LICENSE)).

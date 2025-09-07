# europa1400-networkfix

A network stability patch for *Europa 1400: The Guild* implemented as an `.asi` plugin.

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

## Project structure

```
/
├── bin/                # Compiled plugin
├── src/                # Source code
└── vendor/             # Third-party libraries
```

## Building

Ensure [Zig](https://ziglang.org/) is installed and initialize submodules:

```
git submodule update --init --recursive
make
```

The compiled plugin will be written to `bin/networkfix.asi`.

## Usage

Copy `bin/networkfix.asi` into your Europa 1400 game directory to test the fix.

## License

GPLv3 (see [LICENSE](LICENSE)).

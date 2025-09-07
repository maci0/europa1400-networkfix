# europa1400-networkfix

A network stability patch for *Europa 1400: The Guild* implemented as an `.asi` plugin.
The project builds a Windows dynamic library using [Zig](https://ziglang.org/) and
[MinHook](https://github.com/TsudaKageyu/minhook) to intercept network calls.

## Project structure

```
/
├── bin/                # Compiled plugin
├── scripts/            # Helper scripts
├── src/                # Source code
└── vendor/             # Third-party libraries
```

## Building

Ensure [Zig](https://ziglang.org/) is installed and initialize submodules:

```
git submodule update --init --recursive
./scripts/build.sh
```

The compiled plugin will be written to `bin/networkfix.asi`.

## Usage

Copy `bin/networkfix.asi` into your Europa 1400 game directory to test the fix.

## License

MIT (see [LICENSE](LICENSE)).

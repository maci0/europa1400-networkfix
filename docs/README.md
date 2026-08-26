# Documentation index

New to the project: read the [main README](../README.md), then
[problem-analysis.md](problem-analysis.md) for what breaks and why, then
[architecture.md](architecture.md) for how the plugin fixes it.

| File | Covers | Audience |
|------|--------|----------|
| [problem-analysis.md](problem-analysis.md) | Root cause of the desyncs, why VPNs trigger them, test results | All |
| [architecture.md](architecture.md) | [Hook architecture](architecture.md#hook-architecture), [module components](architecture.md#module-components), [implementation details](architecture.md#hook-implementation-details), [version detection](architecture.md#version-detection) | Developers |
| [development-guide.md](development-guide.md) | [Building](development-guide.md#building-from-source), [debugging](development-guide.md#debugging), [adding hooks](development-guide.md#adding-new-hooks), [code style](development-guide.md#code-style-guidelines), [contributing](development-guide.md#contributing) | Developers |
| [configuration.md](configuration.md) | `game.ini` keys, every `NETWORKFIX_*` / `HARNESS_*` env toggle, log prefixes | Advanced users |
| [server-dll-versions.md](server-dll-versions.md) | Per-version `server.dll` hashes, RVAs, Steam/GOG differences | Developers |
| [../harness/README.md](../harness/README.md) | Headless Docker + Wine testbed: A/B runs, netem packet loss, video capture | Research |
| [../harness/LUA_INTEGRATION.md](../harness/LUA_INTEGRATION.md) | Driving the game from in-process Lua (europa1400-lua sister project) | Research |

## External references

- [MinHook](https://github.com/TsudaKageyu/minhook), the hooking library
- [The-Guild-1-HookDLLs](https://github.com/HarryTheBird/The-Guild-1-HookDLLs), the original research this builds on
- [Winsock reference](https://docs.microsoft.com/en-us/windows/win32/winsock/)

## Editing these docs

Update this index when you add a file, and fix cross-references when you move
content. Run any command you put in a code block before committing it. The
docs are GPLv3 like the code (see [LICENSE](../LICENSE)).

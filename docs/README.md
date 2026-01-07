# Documentation Index

Welcome to the europa1400-networkfix documentation! This directory contains comprehensive technical and user documentation for the network stability plugin.

## Getting Started

**New to the project?** Start here:
1. [Main README](../README.md) - Installation and basic usage
2. [Problem Analysis](problem-analysis.md) - Understanding what this fixes and why
3. [Architecture](architecture.md) - How the plugin works at a technical level

## Documentation by Role

### For Players

If you just want to use the plugin:

- **[Installation Guide](../README.md#installation)** - How to install the plugin
- **[Troubleshooting](../README.md#troubleshooting)** - Common issues and solutions
- **[Configuration](configuration.md)** - Customizing behavior (advanced users)

### For Developers

If you want to build, modify, or contribute:

- **[Development Guide](development-guide.md)** - Complete development workflow
  - [Building from source](development-guide.md#building-from-source)
  - [Debugging](development-guide.md#debugging)
  - [Adding new hooks](development-guide.md#adding-new-hooks)
  - [Contributing](development-guide.md#contributing)
- **[Architecture](architecture.md)** - Technical implementation details
  - [Hook architecture](architecture.md#hook-architecture)
  - [Module components](architecture.md#module-components)
  - [Hook implementation details](architecture.md#hook-implementation-details)
- **[Code Style Guidelines](development-guide.md#code-style-guidelines)** - Coding standards

### For Researchers

If you want to understand the problem and solution:

- **[Problem Analysis](problem-analysis.md)** - Root cause analysis
  - [Technical root cause](problem-analysis.md#technical-root-cause-analysis)
  - [Why VPNs trigger issues](problem-analysis.md#why-vpns-trigger-these-issues)
  - [Solution design](problem-analysis.md#the-solution-targeted-api-hooking)
  - [Testing results](problem-analysis.md#testing-results)
- **[Server DLL Versions](server-dll-versions.md)** - Game version analysis
  - [Function signatures](server-dll-versions.md#function-signature-rva-0x3720)
  - [Version differences](server-dll-versions.md#key-differences-from-steam-version)

## Documentation Files

### Core Documentation

| File | Description | Audience |
|------|-------------|----------|
| [architecture.md](architecture.md) | Technical architecture and hook implementation | Developers |
| [problem-analysis.md](problem-analysis.md) | Root cause analysis and solution design | All |
| [development-guide.md](development-guide.md) | Building, debugging, and contributing | Developers |
| [configuration.md](configuration.md) | Configuration options and tuning | Advanced users |
| [server-dll-versions.md](server-dll-versions.md) | Game version compatibility details | Developers |

### Quick Reference

| Topic | Link |
|-------|------|
| Installation | [README.md#installation](../README.md#installation) |
| Building | [development-guide.md#building-from-source](development-guide.md#building-from-source) |
| Troubleshooting | [README.md#troubleshooting](../README.md#troubleshooting) |
| Hook details | [architecture.md#hook-implementation-details](architecture.md#hook-implementation-details) |
| Adding hooks | [development-guide.md#adding-new-hooks](development-guide.md#adding-new-hooks) |
| Configuration | [configuration.md#configuration-examples](configuration.md#configuration-examples) |
| Version detection | [architecture.md#version-detection](architecture.md#version-detection) |
| Contributing | [development-guide.md#contributing](development-guide.md#contributing) |

## Common Tasks

### I want to...

**...install the plugin**
→ [Installation Steps](../README.md#installation-steps)

**...understand why this is needed**
→ [Problem Analysis](problem-analysis.md)

**...build from source**
→ [Build Instructions](development-guide.md#build-instructions)

**...debug hook issues**
→ [Debugging Guide](development-guide.md#debugging)

**...add support for a new game version**
→ [Version Detection](development-guide.md#version-detection)

**...add a new hook function**
→ [Adding New Hooks](development-guide.md#adding-new-hooks)

**...change retry behavior**
→ [Network Behavior Tuning](configuration.md#network-behavior-tuning)

**...understand how hooks work**
→ [Hook Architecture](architecture.md#hook-architecture)

**...contribute code**
→ [Contributing Guide](development-guide.md#contributing)

**...report a bug**
→ [GitHub Issues](https://github.com/maci0/europa1400-networkfix/issues)

## Documentation Standards

This documentation follows these principles:

1. **User-focused** - Written for the intended audience
2. **Example-driven** - Concrete examples over abstract descriptions
3. **Navigable** - Clear cross-references and table of contents
4. **Maintainable** - Kept in sync with code via file path references
5. **Comprehensive** - Covers all aspects from user to developer

## Contributing to Documentation

Documentation improvements are welcome! Please:

1. Keep language clear and concise
2. Add examples for complex topics
3. Update the index (this file) when adding new docs
4. Test all code examples
5. Update cross-references if moving content

See [Contributing Guidelines](development-guide.md#contributing) for more details.

## Additional Resources

### External Links

- [MinHook Documentation](https://github.com/TsudaKageyu/minhook) - Hooking library used
- [Zig Build System](https://ziglang.org/learn/build-system/) - Build toolchain
- [Windows API Reference](https://docs.microsoft.com/en-us/windows/win32/api/) - Win32 APIs
- [Winsock Documentation](https://docs.microsoft.com/en-us/windows/win32/winsock/) - Network APIs

### Community

- [GitHub Repository](https://github.com/maci0/europa1400-networkfix)
- [Issue Tracker](https://github.com/maci0/europa1400-networkfix/issues)
- [Discussions](https://github.com/maci0/europa1400-networkfix/discussions)

### Related Projects

- [The-Guild-1-HookDLLs](https://github.com/HarryTheBird/The-Guild-1-HookDLLs) - Original research
- [Europa 1400 on Steam](https://store.steampowered.com/app/9900/Europa_1400_The_Guild/)

## License

All documentation is released under the GPLv3 license, same as the project code. See [LICENSE](../LICENSE) for details.

---

**Need help?** Open an [issue](https://github.com/maci0/europa1400-networkfix/issues) or check the [troubleshooting guide](../README.md#troubleshooting).

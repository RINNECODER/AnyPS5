# AnyPS5 for macOS

Apple Silicon PS5 runtime: QEMU/TCG guest CPU, native HLE system libraries, and a Metal graphics backend.

> Fork of [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) (a Linux/Windows relinker); this fork targets macOS.

## Requirements

- A Mac with Apple Silicon (arm64). The CPU runtime build requires it (`ANYPS5_CPU_RUNTIME_ONLY` in [CMakeLists.txt](CMakeLists.txt)).
- Xcode command-line tools with the Metal toolchain. No minimum macOS version is pinned in the build files.

## Get it

- **Engine releases:** signed engine packages are published on the [releases page](https://github.com/RINNECODER/AnyPS5/releases) and announced through the channel feeds (`engine-alpha.json`, `engine-beta.json`, `engine-stable.json` on the [channels release](https://github.com/RINNECODER/AnyPS5/releases/tag/channels)).
- **MacPS:** MacPS, a separate macOS launcher app, installs and updates engines automatically.
- **Build from source:** see the [macOS Metal backend section](docs/dev/BUILD.md#macos-metal-backend) of the build instructions; the exact flags used in CI are in [macos-metal.yml](.github/workflows/macos-metal.yml).

### Release channels

- **alpha:** every green merge to `main`.
- **beta:** promoted automatically after 24 hours and passing the suites.
- **stable:** promoted on maintainer approval.

See [docs/RELEASES.md](docs/RELEASES.md).

## Status

[![alpha](https://rinnecoder.github.io/AnyPS5/badge-engine-alpha.svg)](https://rinnecoder.github.io/AnyPS5/) [![beta](https://rinnecoder.github.io/AnyPS5/badge-engine-beta.svg)](https://rinnecoder.github.io/AnyPS5/) [![stable](https://rinnecoder.github.io/AnyPS5/badge-engine-stable.svg)](https://rinnecoder.github.io/AnyPS5/) [![macOS tests](https://rinnecoder.github.io/AnyPS5/badge-macos-tests.svg)](https://rinnecoder.github.io/AnyPS5/) [![macOS HLE](https://rinnecoder.github.io/AnyPS5/badge-macos-hle.svg)](https://rinnecoder.github.io/AnyPS5/)

[![macOS HLE coverage map](https://rinnecoder.github.io/AnyPS5/macos-coverage.svg)](https://rinnecoder.github.io/AnyPS5/)

<sub>Engine badges: latest version and build date on each channel ("none" if nothing is published). macOS tests: native required tests gating an engine build. macOS HLE: share of the functions declared in [core/libs/prx](core/libs/prx) that a macOS provider in [core/cpu/src](core/cpu/src) registers (static scan; method on the [status page](https://rinnecoder.github.io/AnyPS5/)).</sub>

[List of verified games](docs/user/COMPATIBILITY.md)

Unsupported or unexpected states strictly throw `std::runtime_error`. `what()` is printed to stderr and the process terminates.

## Documentation

[Usage](docs/user/USAGE.md), [Build instructions](docs/dev/BUILD.md), [Architecture](docs/dev/ARCHITECTURE.md), [Technical debt of the project](docs/dev/TechnicalDebt.md), [code style conventions](docs/dev/CONVENTIONS.md), [contributing](CONTRIBUTING.md)

## Compatibility

See the [game compatibility list](docs/user/COMPATIBILITY.md) for tested games and known issues.

## Input mapping

SDL-mapped game controllers are supported, including analog sticks and triggers. Keyboard and mouse controls can be configured with an `anyps5-input.ini` file. See [input mapping](docs/user/INPUT_MAPPING.md) for the supported devices and configuration format.

## Upstream

The upstream project converts executables to native Linux and Windows binaries with a [relinker](core/relinker) and system [prx libraries](core/libs/prx) for dynamic linking, without emulation. That code is kept here so upstream changes sync cleanly; the macOS runtime builds on the same library declarations.

## Disclaimer

This project is intended for interoperability, research, preservation, and compatibility purposes. It does not include, distribute, or require copyrighted software, firmware, cryptographic keys, or proprietary libraries. Users are responsible for ensuring that any binaries used with this project are obtained and used in accordance with applicable laws and their respective license terms.

## License

This project is licensed under the GNU General Public License version 2 only.

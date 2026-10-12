# AnyPS5 for macOS

PS5 runtime for macOS: the guest runs directly under Rosetta 2 as relinked x86_64 code, with native HLE system libraries and a Metal graphics backend.

> Fork of [boykopovar/AnyPS5](https://github.com/boykopovar/AnyPS5) (a Linux/Windows relinker); this fork targets macOS.

## Requirements

- A Mac with Apple Silicon and Rosetta 2 installed (`arch -x86_64 /usr/bin/true` must succeed). The engine is built for x86_64 (`-DANYPS5_ROSETTA_LIBS=ON -DCMAKE_OSX_ARCHITECTURES=x86_64`).
- Xcode command-line tools with the Metal toolchain. No minimum macOS version is pinned in the build files.

## Run a title

The direct path relinks the title with `relinker --macos --to-rosetta`, builds the system libraries as x86_64 Mach-O `libs/<name>.prx`, and runs the result under `arch -x86_64`. [`tools/rosetta/run_title.py`](tools/rosetta/run_title.py) does the relink (cached per title), lays out the run directory and launches it; see [the design](docs/design/rosetta-direct-execution.md) and the [macOS build section](docs/dev/BUILD.md#macos-rosetta-direct-path).

Engine releases are paused while packaging is rebuilt around the Rosetta path. The exact flags CI uses are in [macos-metal.yml](.github/workflows/macos-metal.yml).

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

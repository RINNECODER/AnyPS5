// Fork-only: rebuild a plain ELF from a plaintext SELF container (the C++ form
// of tools/unself.py, with bounds checks). A plain ELF is returned unchanged.
#pragma once

#include <cstdint>
#include <vector>

namespace Aps5Launcher {

// Throws std::runtime_error for encrypted, compressed or malformed containers.
std::vector<std::uint8_t> Unself(const std::vector<std::uint8_t>& data);

}  // namespace Aps5Launcher

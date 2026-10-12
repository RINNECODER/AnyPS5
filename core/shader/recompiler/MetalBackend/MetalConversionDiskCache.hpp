#pragma once

// Persists ConvertToMetal results (the generated MSL and its binding metadata)
// in the shader cache directory, so a launch does not run SPIRV-Cross again
// for shaders an earlier launch converted. Entries live in
// <ShaderCacheDirectory>/msl-<build version>/; the version covers the bridge
// and SPIRV-Cross sources, and each entry is checked against a 192-bit digest
// of its conversion key.

#include "MetalShaderBridge.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace ShaderRecompiler::MetalBackend::ConversionDiskCache {

// Loads the result for `key` into everything but `result.guest`.
[[nodiscard]] bool Load(const std::string& key, Result& result);
// Stores everything but `result.guest`.
void Store(const std::string& key, const Result& result);

[[nodiscard]] std::vector<std::byte> EncodeEntry(const std::string& key, const Result& result);
[[nodiscard]] bool DecodeEntry(std::span<const std::byte> file, const std::string& key, Result& result);

}

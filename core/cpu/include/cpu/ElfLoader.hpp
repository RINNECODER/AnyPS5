#pragma once

#include <cpu/Cpu.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace Cpu {

struct LoadedSegment {
    std::uint64_t Address;
    std::uint64_t FileSize;
    std::uint64_t MemorySize;
    Permission Permissions;
};

struct LoadedImage {
    std::uint64_t Entry = 0;
    std::uint64_t StackPointer = 0;
    std::uint64_t ProgramHeaderAddress = 0;
    std::uint64_t ProgramHeaderSize = 0;
    std::uint64_t ProgramHeaderCount = 0;
    std::uint64_t StackBase = 0;
    std::uint64_t StackSize = 0;
    std::filesystem::path Path;
    std::vector<LoadedSegment> Segments;
};

LoadedImage Load(Machine& machine, const std::filesystem::path& path);
void SetupStack(Machine& machine, LoadedImage& image, const std::vector<std::string>& arguments,
                const std::vector<std::string>& environment = {});

}

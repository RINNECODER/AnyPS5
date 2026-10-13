#ifndef RELINKER_DOMAIN_RELINKRESULT_HPP
#define RELINKER_DOMAIN_RELINKRESULT_HPP

#include <relinker/domain/Types.hpp>
#include <array>
#include <cstdint>
#include <vector>

namespace Relinker {

struct RelinkPatch {
    FileByteOffset Offset;
    std::vector<std::uint8_t> Bytes;
};

struct RelinkResult {
    std::vector<CallRegistryEntry> RegistryEntries;
    std::vector<ProgramHeader> OriginalHeaders;
    SysVDynamicSection DynamicSection;
    VirtualAddress OriginalPltGotVaddr;
    std::vector<RelinkPatch> Patches;
    // One record per `int3` site the opt-in indirect tracer wrote
    // (ANYPS5_TRACE_INDIRECT), so the runtime can emulate the original branch
    // without reading the overwritten bytes. Empty unless the tracer is on.
    struct IndirectTraceSite {
        VirtualAddress OpcodeAddress = 0;
        std::uint32_t PrefixLength = 0;
        std::uint32_t Length = 0;
        std::array<std::uint8_t, 15> Bytes{};
    };
    std::vector<IndirectTraceSite> IndirectTraceSites;
    // Guest range of the executable segments the tracer scanned, for the runtime
    // to discard targets outside the main image.
    VirtualAddress ExecutableBegin = 0;
    VirtualAddress ExecutableEnd = 0;
};

}

#endif

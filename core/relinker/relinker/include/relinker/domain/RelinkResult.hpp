#ifndef RELINKER_DOMAIN_RELINKRESULT_HPP
#define RELINKER_DOMAIN_RELINKRESULT_HPP

#include <relinker/domain/Types.hpp>
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
    // Guest addresses of the `int3` sites the opt-in indirect tracer wrote
    // (ANYPS5_TRACE_INDIRECT). Empty unless the tracer is enabled.
    std::vector<VirtualAddress> IndirectTraceSites;
    // Guest range of the executable segments the tracer scanned, for the runtime
    // to discard targets outside the main image.
    VirtualAddress ExecutableBegin = 0;
    VirtualAddress ExecutableEnd = 0;
};

}

#endif

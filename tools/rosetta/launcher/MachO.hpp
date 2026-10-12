// Fork-only Mach-O helpers for anyps5_rosetta_run (tools/rosetta/launcher):
// read the imports, dylib loads and exports of the relinked x86_64 images, and
// write the small stub dylibs the launcher injects, without clang or dyld_info
// on the user's Mac.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Aps5Launcher {

struct MachOImage {
    std::set<std::string> binds;       // imported symbol names (classic bind opcodes)
    std::vector<std::string> dylibs;   // LC_LOAD_*DYLIB install names
    std::set<std::string> exports;     // export-trie names
};

// Throws std::runtime_error when `path` is not a thin x86_64 Mach-O image.
MachOImage ReadMachO(const std::string& path);

// Names in an export trie (LC_DYLD_INFO export_off or LC_DYLD_EXPORTS_TRIE).
std::set<std::string> ParseExportTrie(const std::uint8_t* trie, std::size_t size);

struct ExportEntry {
    std::string name;
    std::uint64_t address;
};
// A minimal export trie for `entries` (names must be distinct).
std::vector<std::uint8_t> BuildExportTrie(std::vector<ExportEntry> entries);

struct StubDylib {
    std::string installName;
    // Absolute path of the prebuilt stub runtime (tools/rosetta/stub_runtime.c).
    // Empty for a placeholder library: no stubs, no tables, no dependencies.
    std::string runtimePath;
    std::vector<std::string> stubs;        // symbol -> movl $index, %eax; jmp *_aps5_stub_common
    std::vector<std::string> stubLabels;   // logged names, parallel to stubs
    std::vector<std::string> traced;       // symbol -> movl $index, %r11d; jmp *_aps5_trace_common
    std::vector<std::string> tracedLabels; // parallel to traced
};

// The bytes of an x86_64 MH_DYLIB equivalent to run_title.py's clang-built
// aps5_unresolved.dylib: the stubs plus the aps5_stub_names / aps5_trace_names /
// aps5_trace_symbols tables and counts the stub runtime reads.
std::vector<std::uint8_t> BuildStubDylib(const StubDylib& spec);

}  // namespace Aps5Launcher

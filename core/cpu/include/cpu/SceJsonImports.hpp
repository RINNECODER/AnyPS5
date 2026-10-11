#pragma once

#include <cpu/Cpu.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace Cpu {

struct SceImport;

// The sce::Json C++ API of libSceJson / libSceJson2 for guests running under TCG.
//
// The guest owns 8-byte Value, String, Array and Object slots with the layout of
// upstream core/libs/prx/libSceJson2 (a single pointer each). Everything behind a
// slot lives in a provider-owned guest heap, not in host memory, because the guest
// dereferences what the API hands back: c_str() text, the String/Array/Object a
// getter returns a reference to, the Value an index or back() returns.
//
//   Node   (0x38): type i32 @0, bool @4, int64 @8, uint64 @0x10, double @0x18,
//                  String @0x20, Array @0x28, Object @0x30 (upstream Node layout)
//   String rep   : length u64 @0, capacity u64 @8, NUL-terminated text @0x10
//   List header  : size u64 @0, head @8, tail @0x10
//   Array element: next @0, prev @8, Value @0x10
//   Object pair  : next @0, prev @8, Pair{String key @0x10, reserved @0x18, Value @0x20}
//
// Like upstream, MemAllocator/InitParameter2 allocators are recorded but never
// called: every payload comes from the provider heap. Parsing and serialising run
// upstream's pure parser and serialiser on host copies of the guest tree.
// Unknown NIDs return nullopt. Machine must outlive this provider.
class SceJsonImports {
public:
    static constexpr std::uint64_t DefaultGateBase = 0x7ffdd4000000;
    explicit SceJsonImports(Machine& machine, std::uint64_t gateBase = DefaultGateBase);
    ~SceJsonImports();
    SceJsonImports(const SceJsonImports&) = delete;
    SceJsonImports& operator=(const SceJsonImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);

    // Provider heap range and live block count, for tests and diagnostics.
    std::uint64_t HeapBase() const;
    std::uint64_t HeapEnd() const;
    std::size_t LiveAllocations() const;
    // Allocator and user data the guest last passed to Initializer::initialize.
    std::uint64_t RegisteredAllocator() const;
    std::uint64_t RegisteredAllocatorUserData() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}

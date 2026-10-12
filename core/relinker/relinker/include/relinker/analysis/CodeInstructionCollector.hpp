#ifndef RELINKER_CODEINSTRUCTIONCOLLECTOR_HPP
#define RELINKER_CODEINSTRUCTIONCOLLECTOR_HPP

#include <domain/Types.hpp>
#include <algorithm>
#include <set>
#include <utility>
#include <vector>

namespace Relinker {

// Instruction start addresses in ascending order. A sorted vector: a large
// title has tens of millions of instructions, too many for a node-based set.
class CodeInstructionSet {
public:
    using const_iterator = std::vector<Domain::VirtualAddress>::const_iterator;

    CodeInstructionSet() = default;
    explicit CodeInstructionSet(std::vector<Domain::VirtualAddress> sorted) : _addresses(std::move(sorted)) {}

    [[nodiscard]] const_iterator begin() const { return _addresses.begin(); }
    [[nodiscard]] const_iterator end() const { return _addresses.end(); }
    [[nodiscard]] std::size_t size() const { return _addresses.size(); }
    [[nodiscard]] bool empty() const { return _addresses.empty(); }
    [[nodiscard]] const_iterator lower_bound(Domain::VirtualAddress address) const { return std::lower_bound(_addresses.begin(), _addresses.end(), address); }
    [[nodiscard]] bool contains(Domain::VirtualAddress address) const { return std::binary_search(_addresses.begin(), _addresses.end(), address); }

private:
    std::vector<Domain::VirtualAddress> _addresses;
};

class CodeInstructionCollector {
public:
    CodeInstructionSet Collect(const std::vector<std::uint8_t>& bytes, const std::vector<Domain::ProgramHeader>& headers) const;
};

}

#endif

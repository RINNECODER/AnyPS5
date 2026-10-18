// ShaderRegistry container members, kept apart from ShaderRegistry.cpp so the Metal replay build,
// which does not compile the Vulkan shader preparation, can link the registry too.
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>

namespace AgcDriver::DriverDetail {

std::size_t ShaderRegistry::chunkFor(std::uint64_t address) const {
    const auto above = std::upper_bound(chunks.begin(), chunks.end(), address, [](std::uint64_t key, const std::shared_ptr<Chunk>& chunk) { return key < chunk->begin()->first; });
    return above == chunks.begin() ? 0u : static_cast<std::size_t>(above - chunks.begin()) - 1u;
}

ShaderRegistry::Chunk& ShaderRegistry::writable(std::size_t index) {
    auto& chunk = chunks[index];
    if (chunk.use_count() != 1) chunk = std::make_shared<Chunk>(*chunk);
    return *chunk;
}

ShaderRegistry::const_iterator ShaderRegistry::find(std::uint64_t address) const {
    if (chunks.empty()) return end();
    const auto index = chunkFor(address);
    const auto found = chunks[index]->find(address);
    if (found == chunks[index]->end()) return end();
    return const_iterator(chunks.cbegin() + static_cast<std::ptrdiff_t>(index), chunks.cend(), found);
}

ShaderRegistry::const_iterator ShaderRegistry::upper_bound(std::uint64_t address) const {
    if (chunks.empty()) return end();
    const auto index = chunkFor(address);
    const auto found = chunks[index]->upper_bound(address);
    if (found != chunks[index]->end()) return const_iterator(chunks.cbegin() + static_cast<std::ptrdiff_t>(index), chunks.cend(), found);
    if (index + 1u == chunks.size()) return end();
    return const_iterator(chunks.cbegin() + static_cast<std::ptrdiff_t>(index + 1u), chunks.cend(), chunks[index + 1u]->cbegin());
}

const ShaderRegistry::mapped_type& ShaderRegistry::at(std::uint64_t address) const {
    const auto found = find(address);
    if (found == end()) throw std::out_of_range("shader registry has no entry for the code address");
    return found->second;
}

ShaderRegistry::mapped_type& ShaderRegistry::operator[](std::uint64_t address) {
    if (chunks.empty()) chunks.push_back(std::make_shared<Chunk>());
    const auto index = chunks.front()->empty() ? 0u : chunkFor(address);
    auto& chunk = writable(index);
    const auto [entry, inserted] = chunk.try_emplace(address);
    auto& headers = entry->second;
    if (!inserted) return headers;
    ++count;
    if (chunk.size() > ChunkEntries) {
        auto upper = std::make_shared<Chunk>();
        for (auto moved = std::next(chunk.begin(), static_cast<std::ptrdiff_t>(chunk.size() / 2u)); moved != chunk.end();) upper->insert(upper->end(), chunk.extract(moved++));
        chunks.insert(chunks.begin() + static_cast<std::ptrdiff_t>(index + 1u), std::move(upper));
    }
    return headers;
}

std::pair<ShaderRegistry::const_iterator, bool> ShaderRegistry::emplace(std::uint64_t address, mapped_type headers) {
    if (const auto found = find(address); found != end()) return {found, false};
    (*this)[address] = std::move(headers);
    return {find(address), true};
}

std::size_t ShaderRegistry::erase(std::uint64_t address) {
    if (!contains(address)) return 0;
    const auto index = chunkFor(address);
    auto& chunk = writable(index);
    chunk.erase(address);
    --count;
    if (chunk.empty()) chunks.erase(chunks.begin() + static_cast<std::ptrdiff_t>(index));
    return 1;
}

}

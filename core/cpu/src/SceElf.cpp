#include <cpu/SceElf.hpp>
#include <cpu/Self.hpp>
#include <cpu/SceTls.hpp>
#include "SceImageData.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace Cpu {

namespace {
constexpr std::uint64_t PageSize = 4096;
constexpr std::uint64_t AddressLimit = 0x7ffd00000000;
constexpr std::uint64_t StackTop = 0x7ffeff000000;
constexpr std::uint64_t StackSize = 1024 * 1024;
constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("SCE ELF loader: " + message);
}

std::array<std::byte, 32> sourceSha256(std::span<const std::byte> bytes) {
    CC_SHA256_CTX context;
    if (CC_SHA256_Init(&context) != 1) fail("cannot initialize source SHA-256");
    while (!bytes.empty()) {
        const auto length = std::min<std::size_t>(bytes.size(), 1024 * 1024);
        if (CC_SHA256_Update(&context, bytes.data(), static_cast<CC_LONG>(length)) != 1)
            fail("cannot hash source bytes");
        bytes = bytes.subspan(length);
    }
    std::array<std::byte, 32> digest{};
    if (CC_SHA256_Final(reinterpret_cast<unsigned char*>(digest.data()), &context) != 1)
        fail("cannot finalize source SHA-256");
    return digest;
}

bool fits(std::uint64_t offset, std::uint64_t size, std::uint64_t end) {
    return offset <= end && size <= end - offset;
}

std::uint64_t read(const std::vector<std::byte>& bytes, std::uint64_t offset, unsigned size) {
    if (!fits(offset, size, bytes.size())) fail("truncated ELF data");
    std::uint64_t result = 0;
    for (unsigned index = 0; index < size; ++index)
        result |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + index])) << (index * 8);
    return result;
}

std::uint64_t rounded(std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - PageSize + 1) fail("page address overflow");
    return (value + PageSize - 1) & ~(PageSize - 1);
}

Permission permissions(std::uint32_t flags) {
    return static_cast<Permission>(((flags & 4) ? 1 : 0) | ((flags & 2) ? 2 : 0) | ((flags & 1) ? 4 : 0));
}

bool loadable(const SceSegment& segment) {
    return segment.Type == 1 || segment.Type == 0x61000010;
}

struct PageFragment {
    std::uint64_t Address;
    std::size_t Size;
    Permission Permissions;
};

struct PagePlan {
    std::uint64_t Address;
    Permission Permissions;
    bool Mixed;
    std::vector<PageFragment> Fragments;
};

std::vector<PagePlan> pagePlan(const SceParsedImage& image) {
    std::map<std::uint64_t, std::vector<PageFragment>> pages;
    for (const auto& segment : image.Segments) {
        if (!loadable(segment) || !segment.MemorySize) continue;
        const auto end = segment.Address + segment.MemorySize;
        for (auto page = segment.Address & ~(PageSize - 1); page < end; page += PageSize) {
            const auto begin = std::max(page, segment.Address);
            const auto last = std::min(page + PageSize, end);
            pages[page].push_back({begin, static_cast<std::size_t>(last - begin), permissions(segment.Flags)});
        }
    }
    std::vector<PagePlan> result;
    result.reserve(pages.size());
    for (auto& [address, fragments] : pages) {
        std::sort(fragments.begin(), fragments.end(), [](const PageFragment& left, const PageFragment& right) {
            return left.Address < right.Address;
        });
        auto cursor = address;
        for (const auto& fragment : fragments) {
            if (fragment.Address < cursor) fail("overlapping logical PT_LOAD ranges");
            cursor = fragment.Address + fragment.Size;
        }
        const auto first = fragments.front().Permissions;
        const auto mixed = std::any_of(fragments.begin(), fragments.end(), [&](const PageFragment& fragment) {
            return fragment.Permissions != first;
        });
        if (mixed && std::any_of(fragments.begin(), fragments.end(), [](const PageFragment& fragment) {
            return (static_cast<unsigned>(fragment.Permissions) & 4) != 0;
        })) fail("mixed executable shared guest pages are unsupported");
        std::vector<PageFragment> plan;
        if (mixed) {
            plan.reserve(fragments.size() * 2 + 1);
            const auto append = [&](PageFragment fragment) {
                if (!plan.empty() && plan.back().Address + plan.back().Size == fragment.Address &&
                    plan.back().Permissions == fragment.Permissions) plan.back().Size += fragment.Size;
                else plan.push_back(fragment);
            };
            cursor = address;
            for (const auto& fragment : fragments) {
                if (cursor < fragment.Address)
                    append({cursor, static_cast<std::size_t>(fragment.Address - cursor), static_cast<Permission>(0)});
                append(fragment);
                cursor = fragment.Address + fragment.Size;
            }
            if (cursor < address + PageSize)
                append({cursor, static_cast<std::size_t>(address + PageSize - cursor), static_cast<Permission>(0)});
        }
        result.push_back({address, mixed ? static_cast<Permission>(0) : first, mixed, std::move(plan)});
    }
    for (auto& page : result) {
        const auto protectedPage = std::any_of(image.Data->Relro.begin(), image.Data->Relro.end(), [&](const SceSegment& relro) {
            return page.Address >= relro.Address && page.Address - relro.Address < relro.MemorySize;
        });
        if (!protectedPage) continue;
        if (!page.Mixed) page.Permissions = Permission::Read;
        else for (auto& fragment : page.Fragments)
            if (static_cast<unsigned>(fragment.Permissions)) fragment.Permissions = Permission::Read;
    }
    return result;
}

std::uint16_t identifier(std::string_view encoded) {
    if (encoded.empty() || encoded.size() > 3) fail("invalid scoped symbol identifier");
    unsigned result = 0;
    for (const char character : encoded) {
        const auto digit = Alphabet.find(character);
        if (digit == std::string_view::npos || result > (65535 - digit) / 64)
            fail("invalid or overflowing scoped symbol identifier");
        result = result * 64 + static_cast<unsigned>(digit);
    }
    return static_cast<std::uint16_t>(result);
}

void writeWord(Machine& machine, std::uint64_t address, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (unsigned index = 0; index < bytes.size(); ++index) bytes[index] = static_cast<std::byte>(value >> (8 * index));
    machine.Write(address, bytes);
}

std::uint64_t tlsOffset(const SceImageData::Relocation& relocation, const SceImageData::Symbol& symbol,
                        const std::optional<SceSegment>& tls) {
    if (!tls) fail("TLS relocation requires a nonempty main-module TLS segment");
    if (relocation.Type == 16) {
        if (relocation.Addend != 0) fail("DTPMOD64 addend must be zero");
        if (relocation.Symbol && (symbol.Type != 6 || !symbol.Section || symbol.Section >= 0xff00 || symbol.Import))
            fail("TLS relocation requires a defined ordinary-section TLS symbol");
        return 0;
    }
    if (!tls->MemorySize) fail("TLS relocation requires a nonempty main-module TLS segment");
    if (relocation.Symbol == 0) {
        if (relocation.Addend < 0 || static_cast<std::uint64_t>(relocation.Addend) >= tls->MemorySize)
            fail("TLS relocation addend is outside the main module");
        return static_cast<std::uint64_t>(relocation.Addend);
    }
    if (!relocation.Symbol || symbol.Type != 6 || !symbol.Section || symbol.Section >= 0xff00 || symbol.Import)
        fail("TLS relocation requires a defined ordinary-section TLS symbol");
    if (symbol.Value >= tls->MemorySize) fail("TLS relocation symbol offset is outside the main module");
    std::uint64_t offset;
    if (relocation.Addend < 0) {
        const auto magnitude = 0 - static_cast<std::uint64_t>(relocation.Addend);
        if (magnitude > symbol.Value) fail("TLS relocation addend is outside the main module");
        offset = symbol.Value - magnitude;
    } else {
        const auto addend = static_cast<std::uint64_t>(relocation.Addend);
        if (addend >= tls->MemorySize - symbol.Value) fail("TLS relocation addend is outside the main module");
        offset = symbol.Value + addend;
    }
    return offset;
}

}

SceParsedImage ParseSce(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) fail("cannot open " + path.string());
    const auto size = file.tellg();
    if (size < 64 || size > 256 * 1024 * 1024) fail("input size exceeds supported 64-byte to 256-MiB range");
    auto data = std::make_shared<SceImageData>();
    auto& bytes = data->Bytes;
    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) fail("cannot read complete executable");
    data->SourceSize = bytes.size();
    data->SourceSha256 = sourceSha256(bytes);
    std::vector<std::string> reconstructionNotes;
    const bool fromSelf = IsSelf(bytes);
    if (fromSelf) {
        auto decoded = DecodePlainSelf(bytes);
        bytes = std::move(decoded.Bytes);
        reconstructionNotes = std::move(decoded.NormalizationNotes);
    }
    const auto magic = read(bytes, 0, 4);
    if (magic != 0x464c457f || read(bytes, 4, 1) != 2 || read(bytes, 5, 1) != 1 || read(bytes, 6, 1) != 1)
        fail("requires little-endian ELF64 version 1");
    if (read(bytes, 18, 2) != 62 || read(bytes, 20, 4) != 1 || read(bytes, 52, 2) != 64)
        fail("requires a valid x86-64 ELF header");
    SceParsedImage image;
    const auto block = [&](const std::string& reason, SceRequirement requirement = SceRequirement::None) {
        image.UnsupportedReasons.push_back(reason);
        data->Blockers.push_back({requirement, reason});
    };
    image.Path = path;
    image.SourceSha256 = data->SourceSha256;
    image.SourceSize = data->SourceSize;
    image.SourceContainer = fromSelf ? "plain_self" : "elf";
    image.ReconstructionNotes = std::move(reconstructionNotes);
    image.Type = static_cast<std::uint16_t>(read(bytes, 16, 2));
    image.OsAbi = static_cast<std::uint8_t>(read(bytes, 7, 1));
    image.AbiVersion = static_cast<std::uint8_t>(read(bytes, 8, 1));
    image.Entry = read(bytes, 24, 8);
    if (image.Type != 3 && image.Type != 0xfe10 && image.Type != 0xfe18) fail("unsupported SCE executable type");
    if ((image.OsAbi != 0 && image.OsAbi != 9) || image.AbiVersion > 2) fail("unsupported SCE OS ABI or ABI version");
    const auto phOffset = read(bytes, 32, 8);
    const auto phCount = read(bytes, 56, 2);
    if (read(bytes, 54, 2) != 56 || phCount == 0 || phCount > 1024 || !fits(phOffset, phCount * 56, bytes.size()))
        fail("invalid program header table");
    std::optional<SceSegment> dynamic, dynlib;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> mappedRanges;
    for (std::uint64_t index = 0; index < phCount; ++index) {
        const auto offset = phOffset + index * 56;
        SceSegment segment{static_cast<std::uint32_t>(read(bytes, offset, 4)),
            static_cast<std::uint32_t>(read(bytes, offset + 4, 4)), read(bytes, offset + 8, 8),
            read(bytes, offset + 16, 8), read(bytes, offset + 32, 8),
            read(bytes, offset + 40, 8), read(bytes, offset + 48, 8)};
        if (!fits(segment.Offset, segment.FileSize, bytes.size())) fail("program segment exceeds input file");
        if (segment.Flags & ~7u) fail("unsupported program segment permissions");
        if (loadable(segment)) {
            if (segment.FileSize > segment.MemorySize || !fits(segment.Address, segment.MemorySize, AddressLimit))
                fail("invalid PT_LOAD memory range");
            if (segment.Alignment > 1 && (!std::has_single_bit(segment.Alignment) ||
                segment.Offset % segment.Alignment != segment.Address % segment.Alignment)) fail("invalid PT_LOAD alignment");
            if (segment.Offset % PageSize != segment.Address % PageSize) fail("PT_LOAD is not page congruent");
            if (segment.MemorySize == 0) continue;
            const auto begin = segment.Address & ~(PageSize - 1);
            const auto end = rounded(segment.Address + segment.MemorySize);
            for (const auto& other : image.Segments) {
                if (!loadable(other)) continue;
                if (segment.Address < other.Address + other.MemorySize &&
                    other.Address < segment.Address + segment.MemorySize) fail("overlapping logical PT_LOAD ranges");
                if (begin < rounded(other.Address + other.MemorySize) && (other.Address & ~(PageSize - 1)) < end &&
                    segment.Flags != other.Flags) {
                    if ((segment.Flags | other.Flags) & 1)
                        block("mixed executable shared guest pages are unsupported");
#if !ANYPS5_CPU_MODERN_TCG
                    else block("Unicorn exact shared guest data page permissions are unsupported");
#endif
                }
            }
            mappedRanges.emplace_back(begin, end);
            if (segment.Type == 0x61000010) {
                if ((segment.Flags & 4) == 0 || (segment.Flags & 1)) fail("SCE RELRO requires nonexecutable readable memory");
                auto relro = segment;
                relro.Address = begin;
                relro.MemorySize = end - begin;
                data->Relro.push_back(relro);
            }
        } else if (segment.Type == 2) {
            if (dynamic || segment.FileSize == 0 || segment.FileSize % 16) fail("invalid or duplicate dynamic segment");
            dynamic = segment;
        } else if (segment.Type == 0x61000000) {
            if (dynlib) fail("duplicate SCE dynamic data segment");
            dynlib = segment;
        } else if (segment.Type == 7) {
            if (image.Tls || segment.FileSize > segment.MemorySize ||
                (segment.Alignment > 1 && !std::has_single_bit(segment.Alignment))) fail("invalid or duplicate TLS segment");
            image.Tls = segment;
        } else if (segment.Type == 0x61000001) {
            if (image.ProcParam || segment.FileSize < 0x40) fail("invalid or duplicate process parameter segment");
            image.ProcParam = segment;
        } else if (segment.Type == 0x6474e552) {
            data->Relro.push_back(segment);
        } else if (segment.Type == 0x6474e551) {
            if (segment.Flags & 1) fail("executable guest stack is unsupported");
        } else if (segment.Type != 0 && segment.Type != 4 && segment.Type != 6 && segment.Type != 0x6474e550 &&
                   segment.Type != 0x61000002 && segment.Type != 0x61000003 &&
                   segment.Type != 0x6fffff00 && segment.Type != 0x6fffff01) {
            fail("unsupported program header type " + std::to_string(segment.Type));
        }
        image.Segments.push_back(segment);
    }
    std::sort(mappedRanges.begin(), mappedRanges.end());
    std::uint64_t mappedSize = 0, mappedEnd = 0;
    for (const auto& [begin, end] : mappedRanges) {
        const auto first = std::max(begin, mappedEnd);
        if (end > first) {
            if (end - first > 1024 * 1024 * 1024 - mappedSize) fail("image mappings exceed 1-GiB limit");
            mappedSize += end - first;
        }
        mappedEnd = std::max(mappedEnd, end);
    }
    if (!mappedSize || !dynamic) fail("missing load or dynamic segment");
    const auto mapped = [&](std::uint64_t address, std::uint64_t length, std::uint32_t flags, bool backed = false) {
        for (const auto& segment : image.Segments) {
            const auto extent = backed ? segment.FileSize : segment.MemorySize;
            const auto accessible = segment.Flags | (segment.Type == 0x61000010 ? 2u : 0u);
            if (loadable(segment) && (accessible & flags) == flags && address >= segment.Address &&
                fits(address - segment.Address, length, extent)) return;
        }
        fail("unmapped or inaccessible guest range at " + std::to_string(address));
    };
    const auto translate = [&](std::uint64_t address, std::uint64_t length) {
        for (const auto& segment : image.Segments)
            if (loadable(segment) && address >= segment.Address && fits(address - segment.Address, length, segment.FileSize))
                return segment.Offset + address - segment.Address;
        fail("dynamic virtual address is outside file-backed PT_LOAD memory");
        return std::uint64_t{};
    };
    if (image.Type != 0xfe18 || image.Entry != 0) mapped(image.Entry, 1, 1, true);
    if (image.ProcParam) mapped(image.ProcParam->Address, image.ProcParam->FileSize, 4, true);
    if (image.Tls) {
        const auto& tls = *image.Tls;
        if (tls.MemorySize) {
            const auto alignment = std::max<std::uint64_t>(tls.Alignment, 1);
            if (tls.Address % alignment || tls.Offset % alignment) fail("unsupported TLS alignment residue");
            if ((tls.Flags & 4) == 0) fail("TLS template must be readable");
            mapped(tls.Address, tls.MemorySize, 4);
            if (tls.FileSize) {
                mapped(tls.Address, tls.FileSize, 4, true);
                if (translate(tls.Address, tls.FileSize) != tls.Offset) fail("TLS template file mapping does not match its guest address");
            }
        }
    }
    for (const auto& segment : data->Relro) {
        if (segment.MemorySize == 0 || segment.Address % PageSize || segment.MemorySize % PageSize)
            fail("unaligned or empty RELRO range is unsupported");
        mapped(segment.Address, segment.MemorySize, 4);
    }
    std::map<std::uint64_t, std::uint64_t> tags;
    std::vector<std::uint64_t> libraries, modules, neededFiles, libraryAttributes;
    std::vector<std::uint64_t> exportLibraries, exportModules, exportLibraryAttributes, filenames;
    bool terminated = false;
    for (std::uint64_t offset = dynamic->Offset; offset < dynamic->Offset + dynamic->FileSize; offset += 16) {
        const auto tag = read(bytes, offset, 8);
        const auto value = read(bytes, offset + 8, 8);
        if (tag == 0) { terminated = true; break; }
        if (tag == 1) { neededFiles.push_back(value); continue; }
        if (tag == 0x61000015 || tag == 0x61000049) { libraries.push_back(value); continue; }
        if (tag == 0x6100000f || tag == 0x61000045) { modules.push_back(value); continue; }
        if (tag == 0x61000019) { libraryAttributes.push_back(value); continue; }
        if (tag == 0x6100000d || tag == 0x61000043) { exportModules.push_back(value); continue; }
        if (tag == 0x61000013 || tag == 0x61000047) { exportLibraries.push_back(value); continue; }
        if (tag == 0x61000017) { exportLibraryAttributes.push_back(value); continue; }
        if (tag == 0x61000009 || tag == 0x61000041) { filenames.push_back(value); continue; }
        if (tag == 0x61000007) continue;
        if (!tags.emplace(tag, value).second) fail("duplicate dynamic tag " + std::to_string(tag));
    }
    if (!terminated) fail("unterminated dynamic segment");
    const auto get = [&](std::uint64_t standard, std::uint64_t sce) {
        if (tags.contains(standard) == tags.contains(sce)) fail("missing or ambiguous dynamic tag " + std::to_string(standard));
        return tags.at(tags.contains(standard) ? standard : sce);
    };
    const auto table = [&](std::uint64_t standard, std::uint64_t sce, std::uint64_t length) {
        const auto value = get(standard, sce);
        if (tags.contains(standard)) return translate(value, length);
        if (!dynlib || !fits(value, length, dynlib->FileSize)) fail("SCE table exceeds DYNLIBDATA segment");
        return dynlib->Offset + value;
    };
    const auto strSize = get(10, 0x61000037);
    if (strSize == 0) fail("empty dynamic string table");
    const auto strOffset = table(5, 0x61000035, strSize);
    if (read(bytes, strOffset, 1) != 0) fail("invalid dynamic string table null entry");
    const auto string = [&](std::uint64_t offset) {
        if (offset >= strSize) fail("dynamic string offset exceeds table");
        std::string result;
        while (offset < strSize) {
            const auto character = static_cast<char>(read(bytes, strOffset + offset++, 1));
            if (!character) return result;
            result.push_back(character);
        }
        fail("unterminated dynamic string");
        return std::string{};
    };
    struct Identity { std::string Name; std::uint16_t Version; };
    std::map<std::uint16_t, Identity> libraryIds, moduleIds, exportLibraryIds, exportModuleIds;
    const auto identities = [&](const auto& entries, auto& output, const std::string& direction) {
        for (const auto value : entries) {
            const auto id = static_cast<std::uint16_t>(value >> 48);
            const auto name = string(value & 0xffffffff);
            if (name.empty() || name.find_first_of("/\\:#\r\n") != std::string::npos) fail("invalid " + direction + " library or module name");
            Identity identity{name, static_cast<std::uint16_t>(value >> 32)};
            const auto [found, inserted] = output.emplace(id, identity);
            if (!inserted && (found->second.Name != name || found->second.Version != identity.Version))
                fail("conflicting " + direction + " identity for id " + std::to_string(id));
        }
    };
    identities(libraries, libraryIds, "imported");
    identities(modules, moduleIds, "imported");
    identities(exportLibraries, exportLibraryIds, "exported");
    identities(exportModules, exportModuleIds, "exported");
    for (const auto value : exportModules) {
        const auto id = static_cast<std::uint16_t>(value >> 48);
        const auto& identity = exportModuleIds.at(id);
        image.ExportModules.push_back({identity.Name, id, static_cast<std::uint8_t>(identity.Version >> 8),
            static_cast<std::uint8_t>(identity.Version)});
    }
    for (const auto value : exportLibraries) {
        const auto id = static_cast<std::uint16_t>(value >> 48);
        const auto& identity = exportLibraryIds.at(id);
        image.ExportLibraries.push_back({identity.Name, id, identity.Version});
    }
    std::map<std::uint16_t, std::uint64_t> attributesById;
    for (const auto value : libraryAttributes) {
        const auto id = static_cast<std::uint16_t>(value >> 48);
        const auto attributes = value & 0xffffffffffffull;
        if (!libraryIds.contains(id)) fail("attribute has no matching imported library id " + std::to_string(id));
        const auto [found, inserted] = attributesById.emplace(id, attributes);
        if (!inserted && found->second != attributes) fail("conflicting imported library attributes for id " + std::to_string(id));
        image.ImportLibraryAttributes.push_back({id, attributes});
        if (inserted && (attributes & ~0x9ull)) block("imported library attributes are unsupported for execution: id=" +
            std::to_string(id) + " value=" + std::to_string(attributes));
    }
    std::map<std::uint16_t, std::uint64_t> exportAttributesById;
    for (const auto value : exportLibraryAttributes) {
        const auto id = static_cast<std::uint16_t>(value >> 48);
        const auto attributes = value & 0xffffffffffffull;
        if (!exportLibraryIds.contains(id)) fail("attribute has no matching exported library id " + std::to_string(id));
        const auto [found, inserted] = exportAttributesById.emplace(id, attributes);
        if (!inserted && found->second != attributes) fail("conflicting exported library attributes for id " + std::to_string(id));
        image.ExportLibraryAttributes.push_back({id, attributes});
        if (inserted && (attributes & ~0x1ull)) block("exported library attributes are unsupported for execution: id=" +
            std::to_string(id) + " value=" + std::to_string(attributes));
    }
    for (const auto value : filenames) {
        const auto filename = string(value);
        if (image.OriginalFilename && *image.OriginalFilename != filename) fail("conflicting original filename metadata");
        image.OriginalFilename = filename;
    }
    for (const auto& [id, identity] : moduleIds) {
        image.NeededModules.push_back(identity.Name);
        data->NeededModuleIds.emplace(id, identity.Name);
        data->ImportedModules.push_back({identity.Name, id, static_cast<std::uint8_t>(identity.Version >> 8),
            static_cast<std::uint8_t>(identity.Version)});
    }
    for (const auto offset : neededFiles) image.NeededFiles.push_back(string(offset));
    if (!image.NeededFiles.empty()) block("DT_NEEDED guest module loading is unsupported", SceRequirement::Dependencies);
    const auto array = [&](std::uint64_t pointerTag, std::uint64_t sizeTag) {
        SceImageData::Array result;
        if (tags.contains(pointerTag) != tags.contains(sizeTag)) fail("initializer/finalizer array requires paired address and size tags");
        if (!tags.contains(pointerTag)) return result;
        result = {tags.at(pointerTag), tags.at(sizeTag)};
        if (result.Size) {
            if (!result.Address || result.Address % 8 || result.Size % 8 || result.Size > 1024 * 1024)
                fail("invalid initializer/finalizer array range");
            mapped(result.Address, result.Size, 4, true);
        }
        return result;
    };
    data->Init = tags.contains(12) ? tags.at(12) : 0;
    data->Fini = tags.contains(13) ? tags.at(13) : 0;
    if (data->Init) mapped(data->Init, 1, 1, true);
    if (data->Fini) mapped(data->Fini, 1, 1, true);
    data->PreinitArray = array(32, 33);
    data->InitArray = array(25, 27);
    data->FiniArray = array(26, 28);
    for (const auto& [tag, value] : tags) {
        const bool tableTag = tag == 2 || tag == 3 || tag == 4 || (tag >= 5 && tag <= 11) || tag == 14 || tag == 20 || tag == 23 ||
            tag == 0x61000025 || (tag >= 0x61000027 && tag <= 0x6100003f && (tag & 1)) || tag == 0x6ffffff9;
        if (tableTag) continue;
        if (tag == 30 && !(value & ~(8ull | 2ull | 16ull))) {
            if (value & 2) {
                data->Symbolic = true;
                block("guest symbolic binding is unsupported by the single-image loader", SceRequirement::Symbolic);
            }
            if (value & 16) block("guest static TLS graph is unsupported by the single-image loader", SceRequirement::StaticTls);
            continue;
        }
        if (tag == 16 && value == 0) {
            data->Symbolic = true;
            block("guest symbolic binding is unsupported by the single-image loader", SceRequirement::Symbolic);
            continue;
        }
        if (tag == 0x6ffffffb && !(value & ~(1ull | 0x8000000ull))) continue;
        if (tag == 21) continue;
        if (tag == 12 || tag == 13 || (tag >= 25 && tag <= 28) || tag == 32 || tag == 33 ||
            tag == 0x6000000c || tag == 0x6000000d || (tag >= 0x60000019 && tag <= 0x6000001c)) {
            if (value) block("guest initializer or finalizer execution is unsupported",
                tag < 0x60000000 ? SceRequirement::Initializers : SceRequirement::None);
            continue;
        }
        if (tag == 0x61000011 && value == 0) continue;
        block("unsupported dynamic tag " + std::to_string(tag));
    }
    if (get(11, 0x6100003b) != 24) fail("unsupported dynamic symbol entry size");
    std::uint64_t symSize;
    if (tags.contains(0x6100003f)) symSize = tags.at(0x6100003f);
    else if (tags.contains(4)) symSize = read(bytes, translate(tags.at(4), 8) + 4, 4) * 24;
    else fail("missing dynamic symbol count");
    if (!symSize || symSize % 24 || symSize / 24 > 1024 * 1024) fail("invalid dynamic symbol table size");
    const auto symOffset = table(6, 0x61000039, symSize);
    std::map<std::string, std::size_t> importedNames;
    for (std::uint64_t offset = 0; offset < symSize; offset += 24) {
        const auto name = string(read(bytes, symOffset + offset, 4));
        const auto info = read(bytes, symOffset + offset + 4, 1);
        const auto visibility = read(bytes, symOffset + offset + 5, 1);
        SceImageData::Symbol symbol{read(bytes, symOffset + offset + 8, 8), read(bytes, symOffset + offset + 16, 8),
            static_cast<std::uint16_t>(read(bytes, symOffset + offset + 6, 2)), static_cast<std::uint8_t>(info & 15), {},
            static_cast<std::uint8_t>(info >> 4), static_cast<std::uint8_t>(visibility)};
        if (!offset && (info || visibility || symbol.Value || symbol.Size || symbol.Section || !name.empty())) fail("invalid null dynamic symbol");
        if ((info >> 4) > 2 || visibility > 3) fail("unsupported dynamic symbol attributes");
        if (symbol.Type != 0 && symbol.Type != 1 && symbol.Type != 2 && symbol.Type != 6)
            block("unsupported dynamic symbol type " + std::to_string(symbol.Type));
        if (symbol.Type == 6 && symbol.Section &&
            (!image.Tls || symbol.Section >= 0xff00 || !fits(symbol.Value, symbol.Size, image.Tls->MemorySize)))
            fail("TLS symbol exceeds TLS segment or uses an unsupported section");
        if (offset && !symbol.Section && (info >> 4) != 0) {
            const auto first = name.find('#');
            const auto second = first == std::string::npos ? first : name.find('#', first + 1);
            if (first != 11 || second == std::string::npos || name.find('#', second + 1) != std::string::npos)
                fail("import requires full NID#library#module scope: " + name);
            if (name.substr(0, first).find_first_not_of(Alphabet) != std::string::npos) fail("invalid import NID");
            const auto libraryId = identifier(std::string_view(name).substr(first + 1, second - first - 1));
            const auto moduleId = identifier(std::string_view(name).substr(second + 1));
            if (!libraryIds.contains(libraryId) || !moduleIds.contains(moduleId)) fail("import qualifier has no matching library or module metadata: " + name);
            const auto& library = libraryIds.at(libraryId);
            const auto& module = moduleIds.at(moduleId);
            const auto [found, inserted] = importedNames.emplace(name, image.Imports.size());
            if (inserted) image.Imports.push_back({name.substr(0, first), library.Name, libraryId, module.Name, moduleId,
                library.Version, static_cast<std::uint8_t>(module.Version >> 8), static_cast<std::uint8_t>(module.Version)});
            symbol.Import = found->second;
            if (symbol.Type != 2) block("guest data or TLS imports, including untyped imports, are unsupported: " + name,
                symbol.Type == 1 ? SceRequirement::TypedImports : symbol.Type == 6 ? SceRequirement::ImportedTls : SceRequirement::None);
        } else if (symbol.Section && symbol.Section != 0xfff1 && symbol.Type != 6) {
            if (symbol.Section >= 0xff00) fail("unsupported dynamic symbol section");
            mapped(symbol.Value, std::max<std::uint64_t>(symbol.Size, 1), symbol.Type == 2 ? 1 : 0);
        }
        if (symbol.Section && (info >> 4) != 0 && name.find('#') != std::string::npos) {
            const auto first = name.find('#');
            const auto second = name.find('#', first + 1);
            if (first != 11 || second == std::string::npos || name.find('#', second + 1) != std::string::npos)
                fail("export requires full NID#library#module scope: " + name);
            if (name.substr(0, first).find_first_not_of(Alphabet) != std::string::npos) fail("invalid export NID");
            const auto libraryId = identifier(std::string_view(name).substr(first + 1, second - first - 1));
            const auto moduleId = identifier(std::string_view(name).substr(second + 1));
            if (!exportLibraryIds.contains(libraryId) || !exportModuleIds.contains(moduleId))
                fail("export qualifier has no matching library or module metadata: " + name);
            const auto& library = exportLibraryIds.at(libraryId);
            const auto& module = exportModuleIds.at(moduleId);
            image.Exports.push_back({{name.substr(0, first), library.Name, libraryId, module.Name, moduleId,
                library.Version, static_cast<std::uint8_t>(module.Version >> 8), static_cast<std::uint8_t>(module.Version)},
                offset / 24, symbol.Value, symbol.Size, symbol.Section, symbol.Type, static_cast<std::uint8_t>(info >> 4),
                static_cast<std::uint8_t>(visibility)});
            if (symbol.Type == 0) block("untyped guest exports are unsupported for execution: " + name);
        }
        data->Symbols.push_back(symbol);
    }
    std::map<std::uint64_t, std::uint64_t> targets;
    const auto relocations = [&](std::uint64_t pointer, std::uint64_t scePointer, std::uint64_t lengthTag, std::uint64_t sceLength, bool plt) {
        if (!tags.contains(pointer) && !tags.contains(scePointer) && !tags.contains(lengthTag) && !tags.contains(sceLength)) return;
        const auto length = get(lengthTag, sceLength);
        if (length % 24) fail("invalid RELA table size");
        const auto offset = table(pointer, scePointer, length);
        for (std::uint64_t index = 0; index < length; index += 24) {
            const auto info = read(bytes, offset + index + 8, 8);
            SceImageData::Relocation relocation{read(bytes, offset + index, 8), info >> 32, static_cast<std::uint32_t>(info),
                std::bit_cast<std::int64_t>(read(bytes, offset + index + 16, 8)), plt};
            if (relocation.Symbol >= data->Symbols.size()) fail("relocation symbol index exceeds symbol table");
            if (plt && relocation.Type != 7) fail("unsupported PLT relocation type");
            if (relocation.Type == 1 || relocation.Type == 6 || relocation.Type == 7 || relocation.Type == 8 ||
                relocation.Type == 16 || relocation.Type == 17 || relocation.Type == 18) {
                mapped(relocation.Target, 8, 2);
                const auto next = targets.lower_bound(relocation.Target);
                if ((next != targets.end() && next->first < relocation.Target + 8) ||
                    (next != targets.begin() && std::prev(next)->second > relocation.Target)) fail("overlapping relocation writes");
                targets.emplace(relocation.Target, relocation.Target + 8);
                if (relocation.Type == 8 && relocation.Symbol != 0) fail("RELATIVE relocation must have symbol zero");
                if ((relocation.Type == 6 || relocation.Type == 7) && relocation.Addend != 0) fail("GLOB_DAT/JUMP_SLOT addend must be zero");
                if (relocation.Type <= 7) {
                    if (relocation.Symbol == 0) fail("symbol relocation has null symbol");
                    if (data->Symbols[relocation.Symbol].Type == 6) fail("non-TLS relocation references a TLS symbol");
                }
                if (relocation.Type >= 16) {
                    const auto& symbol = data->Symbols[relocation.Symbol];
                    if (symbol.Import && symbol.Type == 6) {
                        if (relocation.Type == 16 && relocation.Addend != 0) fail("DTPMOD64 addend must be zero");
                    } else tlsOffset(relocation, symbol, image.Tls);
                }
            } else block("unsupported relocation type " + std::to_string(relocation.Type));
            if (std::find(image.RelocationTypes.begin(), image.RelocationTypes.end(), relocation.Type) == image.RelocationTypes.end())
                image.RelocationTypes.push_back(relocation.Type);
            data->Relocations.push_back(relocation);
        }
    };
    if ((tags.contains(7) || tags.contains(0x6100002f)) && get(9, 0x61000033) != 24) fail("unsupported RELA entry size");
    relocations(7, 0x6100002f, 8, 0x61000031, false);
    relocations(23, 0x61000029, 2, 0x6100002d, true);
    if ((tags.contains(23) || tags.contains(0x61000029)) && get(20, 0x6100002b) != 7) fail("PLT table is not RELA");
    image.RelocationCount = data->Relocations.size();
    image.Data = std::move(data);
    return image;
}

void RequireSceProfile(const SceParsedImage& image, unsigned fulfilledRequirements) {
    if (!image.Data) fail("missing validated image data");
    for (const auto& blocker : image.Data->Blockers) {
        const auto required = static_cast<unsigned>(blocker.Requirement);
        if (!required || (required & fulfilledRequirements) != required) fail(blocker.Reason);
    }
}

std::uint64_t SceAddress(std::uint64_t bias, std::uint64_t value) {
    if (bias < PageSize || bias % PageSize) fail("load bias must be a nonzero page-aligned guest address");
    if (bias > AddressLimit || value > AddressLimit - bias) fail("relocated address exceeds guest image range");
    return bias + value;
}

void ValidateSceMapping(const SceParsedImage& image, std::uint64_t bias) {
    SceAddress(bias, 0);
    for (const auto& segment : image.Segments)
        if (loadable(segment)) SceAddress(bias, rounded(segment.Address + segment.MemorySize));
}

void MapSceImage(Machine& machine, const SceParsedImage& image, std::uint64_t bias) {
    ValidateSceMapping(image, bias);
    const auto pages = pagePlan(image);
    for (std::size_t index = 0; index < pages.size();) {
        const auto begin = pages[index].Address;
        auto end = begin + PageSize;
        ++index;
        while (index < pages.size() && pages[index].Address == end) {
            end += PageSize;
            ++index;
        }
        machine.Map(SceAddress(bias, begin), end - begin, Permission::Read | Permission::Write);
    }
    for (const auto& segment : image.Segments) {
        if (!loadable(segment) || !segment.MemorySize) continue;
        machine.Write(SceAddress(bias, segment.Address), std::span(image.Data->Bytes).subspan(segment.Offset, segment.FileSize));
    }
}

void ProtectSceImage(Machine& machine, const SceParsedImage& image, std::uint64_t bias) {
    const auto pages = pagePlan(image);
    for (std::size_t index = 0; index < pages.size();) {
        const auto begin = pages[index].Address;
        const auto access = pages[index].Permissions;
        auto end = begin + PageSize;
        const auto mixed = pages[index].Mixed;
        ++index;
        while (index < pages.size() && !mixed && !pages[index].Mixed &&
            pages[index].Address == end && pages[index].Permissions == access) {
            end += PageSize;
            ++index;
        }
        machine.Protect(SceAddress(bias, begin), end - begin, access);
    }
    for (const auto& page : pages)
        if (page.Mixed)
            for (const auto& fragment : page.Fragments)
                machine.ProtectFragment(SceAddress(bias, fragment.Address), fragment.Size, fragment.Permissions);
}

std::vector<SceRelocationWrite> PlanSceRelocations(const SceParsedImage& image, std::uint64_t bias,
    const std::function<SceSymbolValue(std::uint64_t)>& symbolResolver, const SceTls* tls) {
    std::vector<SceRelocationWrite> writes;
    for (const auto& relocation : image.Data->Relocations) {
        std::uint64_t value;
        if (relocation.Type == 8) value = bias + static_cast<std::uint64_t>(relocation.Addend);
        else {
            const auto& source = image.Data->Symbols.at(relocation.Symbol);
            if (relocation.Symbol && !source.Section && !source.Import) fail("unresolved local symbol relocation");
            const auto symbol = symbolResolver(relocation.Symbol);
            if (relocation.Type == 16 || relocation.Type == 17 || relocation.Type == 18) {
                if (!tls || symbol.Type != 6 || !symbol.TlsModuleId) fail("TLS relocation requires initialized typed provider identity");
                const auto size = tls->MemorySize(symbol.TlsModuleId);
                if (relocation.Type == 16) {
                    if (relocation.Addend != 0) fail("DTPMOD64 addend must be zero");
                    value = symbol.TlsModuleId;
                } else {
                    auto offset = symbol.TlsOffset;
                    if (relocation.Addend < 0) {
                        const auto magnitude = 0 - static_cast<std::uint64_t>(relocation.Addend);
                        if (magnitude > offset) fail("TLS relocation addend is outside the provider module");
                        offset -= magnitude;
                    } else {
                        const auto addend = static_cast<std::uint64_t>(relocation.Addend);
                        if (offset >= size || addend >= size - offset) fail("TLS relocation addend is outside the provider module");
                        offset += addend;
                    }
                    if (relocation.Type == 17) value = tls->Dtpoff(symbol.TlsModuleId, offset);
                    else value = static_cast<std::uint64_t>(tls->Tpoff(symbol.TlsModuleId, offset));
                }
            } else {
                if (symbol.Type == 6 || symbol.TlsModuleId || symbol.TlsOffset) fail("non-TLS relocation resolved to TLS storage");
                if (relocation.Type == 7 && symbol.Type != 2) fail("JUMP_SLOT relocation requires a function provider");
                if (relocation.Type != 1 && relocation.Type != 6 && relocation.Type != 7)
                    fail("unsupported relocation type " + std::to_string(relocation.Type));
                value = symbol.Address;
                if (relocation.Type == 1) value += static_cast<std::uint64_t>(relocation.Addend);
            }
        }
        writes.push_back({SceAddress(bias, relocation.Target), value});
    }
    return writes;
}

void ApplySceRelocations(Machine& machine, std::span<const SceRelocationWrite> writes) {
    for (const auto& write : writes) writeWord(machine, write.Address, write.Value);
}

SceLoadedImage LoadSce(Machine& machine, const std::filesystem::path& path, std::uint64_t loadBias,
                      const std::function<std::uint64_t(const SceImport&)>& resolver) {
    const auto parsed = ParseSce(path);
    if (parsed.Type == 0xfe18) fail("guest shared module loading is unsupported");
    RequireSceProfile(parsed, 0);
    ValidateSceMapping(parsed, loadBias);
    const auto& data = *parsed.Data;
    std::set<std::uint16_t> providers;
    for (const auto& import : parsed.Imports) providers.insert(import.ModuleId);
    for (const auto& [id, name] : data.NeededModuleIds)
        if (!providers.contains(id)) fail("guest module initialization/loading is unsupported: " + name + " id=" + std::to_string(id));
    std::vector<std::uint64_t> imports;
    for (const auto& import : parsed.Imports) {
        if (!resolver) fail("host import resolver is required");
        const auto gate = resolver(import);
        if (!gate) fail("unresolved import " + import.Nid + " in " + import.LibraryName + "/" + import.ModuleName);
        machine.CheckAccess(gate, 1, Permission::Execute);
        imports.push_back(gate);
    }
    std::shared_ptr<SceTls> tls;
    if (parsed.Tls)
        tls = std::make_shared<SceTls>(machine, std::span(data.Bytes).subspan(parsed.Tls->Offset, parsed.Tls->FileSize),
            parsed.Tls->MemorySize, parsed.Tls->Alignment);
    const auto writes = PlanSceRelocations(parsed, loadBias, [&](std::uint64_t index) {
        if (!index) return SceSymbolValue{0, 6, 0, tls ? 1ull : 0ull, 0};
        const auto& symbol = data.Symbols.at(index);
        if (symbol.Type == 6) return SceSymbolValue{0, 6, symbol.Size, 1, symbol.Value};
        return SceSymbolValue{symbol.Import ? imports.at(*symbol.Import) : symbol.Section == 0xfff1 ? symbol.Value : SceAddress(loadBias, symbol.Value),
            symbol.Type, symbol.Size, 0, 0};
    }, tls.get());
    MapSceImage(machine, parsed, loadBias);
    ApplySceRelocations(machine, writes);
    if (tls && parsed.Tls->FileSize) {
        std::vector<std::byte> initializedBytes(static_cast<std::size_t>(parsed.Tls->FileSize));
        machine.Read(SceAddress(loadBias, parsed.Tls->Address), initializedBytes);
        machine.Write(tls->TlsBase(), initializedBytes);
    }
    ProtectSceImage(machine, parsed, loadBias);
    SceLoadedImage image{path, SceAddress(loadBias, parsed.Entry), loadBias, 0, 0,
        parsed.Imports, parsed.NeededModules, parsed.ProcParam, std::move(tls)};
    if (image.ProcParam) image.ProcParam->Address = SceAddress(loadBias, image.ProcParam->Address);
    return image;
}

void SetupSceEntry(Machine& machine, SceLoadedImage& image, const std::vector<std::string>& arguments,
                   std::uint64_t exitGate) {
    if (image.StackPointer) fail("SCE entry stack is already configured");
    machine.CheckAccess(exitGate, 1, Permission::Execute);
    std::uint64_t required = 8 * (arguments.size() + 4) + 15;
    for (const auto& argument : arguments) {
        if (argument.find('\0') != std::string::npos) fail("entry arguments contain embedded NUL");
        if (argument.size() >= StackSize || required > StackSize - argument.size() - 1) fail("entry arguments exceed stack size");
        required += argument.size() + 1;
    }
    if (required > StackSize) fail("entry arguments exceed stack size");
    machine.Map(StackTop - StackSize, StackSize, Permission::Read | Permission::Write);
    auto cursor = StackTop;
    std::vector<std::uint64_t> pointers;
    for (const auto& argument : arguments) {
        cursor -= argument.size() + 1;
        machine.Write(cursor, std::as_bytes(std::span(argument.c_str(), argument.size() + 1)));
        pointers.push_back(cursor);
    }
    cursor = ((cursor - 8 * (pointers.size() + 3) - 8) & ~15ull) + 8;
    image.ArgumentBlock = cursor;
    writeWord(machine, cursor, pointers.size());
    for (std::size_t index = 0; index < pointers.size(); ++index) writeWord(machine, cursor + 8 * (index + 1), pointers[index]);
    writeWord(machine, cursor + 8 * (pointers.size() + 1), 0);
    writeWord(machine, cursor + 8 * (pointers.size() + 2), 0);
    image.StackPointer = cursor;
    machine.Set(Register::Rdi, image.ArgumentBlock);
    machine.Set(Register::Rsi, exitGate);
    machine.Set(Register::Rsp, image.StackPointer);
    machine.Set(Register::Rdx, 0);
    machine.Set(Register::Rflags, 2);
}

}

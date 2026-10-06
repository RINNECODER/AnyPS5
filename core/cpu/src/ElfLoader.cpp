#include <cpu/ElfLoader.hpp>
#include <algorithm>
#include <array>
#include <fstream>
#include <random>
#include <stdexcept>

namespace Cpu {
namespace {

constexpr std::uint64_t PageSize = 4096;
constexpr std::uint64_t StackTop = 0x7fff00000000;
constexpr std::uint64_t StackSize = 1024 * 1024;
constexpr std::uint64_t HeaderAddress = 0x7ffe00000000;
constexpr std::uint64_t MaximumImageSize = 256 * 1024 * 1024;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("ELF loader: " + message);
}

bool fits(std::uint64_t offset, std::uint64_t size, std::uint64_t end) {
    return offset <= end && size <= end - offset;
}

std::uint64_t read(const std::vector<std::byte>& bytes, std::uint64_t offset, unsigned size) {
    if (!fits(offset, size, bytes.size())) fail("truncated ELF header or program header");
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset + i])) << (i * 8);
    return value;
}

std::uint64_t pageEnd(std::uint64_t end) {
    return (end + PageSize - 1) & ~(PageSize - 1);
}

struct Segment {
    std::uint64_t Offset, Address, FileSize, MemorySize, MapBegin, MapEnd;
    Permission Permissions;
};

Permission permissions(std::uint64_t flags) {
    if (flags & ~7ull) fail("unsupported segment permission flags");
    unsigned value = 0;
    if (flags & 4) value |= static_cast<unsigned>(Permission::Read);
    if (flags & 2) value |= static_cast<unsigned>(Permission::Write);
    if (flags & 1) value |= static_cast<unsigned>(Permission::Execute);
    return static_cast<Permission>(value);
}

void writeWord(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(static_cast<std::byte>(value >> (i * 8)));
}

}

LoadedImage Load(Machine& machine, const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) fail("cannot open " + path.string());
    const auto fileSize = file.tellg();
    if (fileSize < 64 || fileSize > 64 * 1024 * 1024) fail("file size outside supported 64-byte to 64-MiB range");
    std::vector<std::byte> bytes(static_cast<std::size_t>(fileSize));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), fileSize)) fail("cannot read complete executable");
    if (read(bytes, 0, 4) != 0x464c457f) fail("input is not an ELF executable");
    if (read(bytes, 4, 1) != 2 || read(bytes, 5, 1) != 1 || read(bytes, 6, 1) != 1)
        fail("requires little-endian ELF64 version 1");
    const auto osAbi = read(bytes, 7, 1);
    if ((osAbi != 0 && osAbi != 3) || read(bytes, 8, 1) != 0) fail("unsupported ELF OS ABI");
    if (read(bytes, 16, 2) != 2) fail("only static ET_EXEC executables are supported");
    if (read(bytes, 18, 2) != 62) fail("requires an x86-64 guest executable");
    if (read(bytes, 20, 4) != 1 || read(bytes, 52, 2) != 64) fail("invalid ELF header version or size");
    const auto entry = read(bytes, 24, 8);
    const auto phOffset = read(bytes, 32, 8);
    const auto phSize = read(bytes, 54, 2);
    const auto phCount = read(bytes, 56, 2);
    if (phSize != 56 || phCount == 0 || phCount == 0xffff || !fits(phOffset, phCount * phSize, bytes.size()))
        fail("invalid or unsupported program header table");
    std::vector<Segment> segments;
    std::uint64_t mappedSize = 0;
    for (std::uint64_t i = 0; i < phCount; ++i) {
        const auto offset = phOffset + i * phSize;
        const auto type = read(bytes, offset, 4);
        if (type == 2) fail("PT_DYNAMIC requires an unsupported dynamic linker");
        if (type == 3) fail("PT_INTERP requires an unsupported program interpreter");
        if (type == 7) fail("PT_TLS requires unsupported guest TLS initialization");
        if (type == 0x6474e551) {
            if (read(bytes, offset + 4, 4) & 1) fail("executable guest stacks are unsupported");
            continue;
        }
        if (type == 0 || type == 4 || type == 6 || type == 0x6474e550) continue;
        if (type != 1) fail("unsupported program header type " + std::to_string(type));
        Segment segment{};
        segment.Permissions = permissions(read(bytes, offset + 4, 4));
        segment.Offset = read(bytes, offset + 8, 8);
        segment.Address = read(bytes, offset + 16, 8);
        segment.FileSize = read(bytes, offset + 32, 8);
        segment.MemorySize = read(bytes, offset + 40, 8);
        const auto alignment = read(bytes, offset + 48, 8);
        if (segment.FileSize > segment.MemorySize || !fits(segment.Offset, segment.FileSize, bytes.size()))
            fail("invalid PT_LOAD file range");
        if (!fits(segment.Address, segment.MemorySize, HeaderAddress) || segment.Address < PageSize)
            fail("PT_LOAD address is outside supported low canonical guest memory");
        if ((segment.Address & (PageSize - 1)) != (segment.Offset & (PageSize - 1)))
            fail("PT_LOAD address and file offset are not page congruent");
        if (alignment > 1 && ((alignment & (alignment - 1)) != 0 ||
            (segment.Address & (alignment - 1)) != (segment.Offset & (alignment - 1))))
            fail("invalid PT_LOAD alignment");
        if (segment.MemorySize == 0) continue;
        segment.MapBegin = segment.Address & ~(PageSize - 1);
        segment.MapEnd = pageEnd(segment.Address + segment.MemorySize);
        if (!segments.empty() && segment.Address < segments.back().Address) fail("PT_LOAD addresses are not sorted");
        for (const auto& other : segments)
            if (segment.MapBegin < other.MapEnd && other.MapBegin < segment.MapEnd)
                fail("overlapping PT_LOAD pages are unsupported");
        const auto length = segment.MapEnd - segment.MapBegin;
        if (length > MaximumImageSize - mappedSize) fail("PT_LOAD mappings exceed the supported 256-MiB image limit");
        mappedSize += length;
        segments.push_back(segment);
    }
    if (segments.empty()) fail("executable has no loadable memory");
    const auto executableEntry = std::any_of(segments.begin(), segments.end(), [&](const auto& segment) {
        return (static_cast<unsigned>(segment.Permissions) & static_cast<unsigned>(Permission::Execute)) &&
               entry >= segment.Address && entry - segment.Address < segment.FileSize;
    });
    if (!executableEntry) fail("entry point is not inside file-backed executable PT_LOAD memory");
    LoadedImage image;
    image.Entry = entry;
    image.ProgramHeaderSize = phSize;
    image.ProgramHeaderCount = phCount;
    image.Path = path;
    for (const auto& segment : segments) {
        machine.Map(segment.MapBegin, segment.MapEnd - segment.MapBegin, Permission::Read | Permission::Write);
        machine.Write(segment.Address, std::span(bytes).subspan(segment.Offset, segment.FileSize));
        const std::array<std::byte, PageSize> zeros{};
        auto cursor = segment.Address + segment.FileSize;
        const auto end = segment.Address + segment.MemorySize;
        while (cursor < end) {
            const auto size = std::min<std::uint64_t>(zeros.size(), end - cursor);
            machine.Write(cursor, std::span(zeros).first(size));
            cursor += size;
        }
        machine.Protect(segment.MapBegin, segment.MapEnd - segment.MapBegin, segment.Permissions);
        image.Segments.push_back({segment.Address, segment.FileSize, segment.MemorySize, segment.Permissions});
        if (phOffset >= segment.Offset && fits(phOffset - segment.Offset, phSize * phCount, segment.FileSize) &&
            (static_cast<unsigned>(segment.Permissions) & static_cast<unsigned>(Permission::Read)))
            image.ProgramHeaderAddress = segment.Address + phOffset - segment.Offset;
    }
    if (image.ProgramHeaderAddress == 0) {
        const auto size = pageEnd(phSize * phCount);
        machine.Map(HeaderAddress, size, Permission::Read | Permission::Write);
        machine.Write(HeaderAddress, std::span(bytes).subspan(phOffset, phSize * phCount));
        machine.Protect(HeaderAddress, size, Permission::Read);
        image.ProgramHeaderAddress = HeaderAddress;
    }
    return image;
}

void SetupStack(Machine& machine, LoadedImage& image, const std::vector<std::string>& arguments,
                const std::vector<std::string>& environment) {
    if (image.StackPointer != 0) fail("initial stack has already been configured");
    const auto path = image.Path.string();
    std::uint64_t required = path.size() + 1 + 16 + 15 + 8 * (arguments.size() + environment.size() + 3 + 20);
    for (const auto* strings : {&arguments, &environment}) {
        for (const auto& string : *strings) {
            if (string.find('\0') != std::string::npos) fail("initial stack strings cannot contain embedded NUL bytes");
            if (string.size() >= StackSize || required > StackSize - string.size() - 1) fail("arguments exceed the guest stack size");
            required += string.size() + 1;
        }
    }
    if (required > StackSize) fail("initial stack exceeds the guest stack size");
    machine.Map(StackTop - StackSize, StackSize, Permission::Read | Permission::Write);
    auto cursor = StackTop;
    const auto pushString = [&](const std::string& string) {
        cursor -= string.size() + 1;
        machine.Write(cursor, std::as_bytes(std::span(string.c_str(), string.size() + 1)));
        return cursor;
    };
    const auto execfn = pushString(path);
    std::vector<std::uint64_t> argv, envp;
    for (const auto& argument : arguments) argv.push_back(pushString(argument));
    for (const auto& variable : environment) envp.push_back(pushString(variable));
    std::array<std::byte, 16> randomBytes;
    std::random_device entropy;
    for (auto& byte : randomBytes) byte = static_cast<std::byte>(entropy());
    cursor -= randomBytes.size();
    const auto randomAddress = cursor;
    machine.Write(cursor, randomBytes);
    std::vector<std::byte> words;
    writeWord(words, argv.size());
    for (const auto pointer : argv) writeWord(words, pointer);
    writeWord(words, 0);
    for (const auto pointer : envp) writeWord(words, pointer);
    writeWord(words, 0);
    for (const auto [type, value] : std::array<std::pair<std::uint64_t, std::uint64_t>, 9>{{
        {3, image.ProgramHeaderAddress}, {4, image.ProgramHeaderSize}, {5, image.ProgramHeaderCount},
        {6, PageSize}, {7, 0}, {8, 0}, {9, image.Entry}, {25, randomAddress}, {31, execfn}}}) {
        writeWord(words, type);
        writeWord(words, value);
    }
    writeWord(words, 0);
    writeWord(words, 0);
    cursor = (cursor - words.size()) & ~15ull;
    machine.Write(cursor, words);
    image.StackPointer = cursor;
    image.StackBase = StackTop - StackSize;
    image.StackSize = StackSize;
    machine.Set(Register::Rsp, cursor);
    machine.Set(Register::Rdx, 0);
    machine.Set(Register::Rflags, 2);
}

}

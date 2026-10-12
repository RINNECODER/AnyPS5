// sceKernelGetdents reports a symbolic link as the type of what it names. The
// guest file system has no links, but a host layout of a title can (app0
// linked to its resource root), and a title that walks a directory skips
// entries that are neither DT_DIR nor DT_REG.
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libkernel/File/include/FileFlags.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

extern "C" {
int APS5_VABI sceKernelOpen(const char*, int, std::uint16_t);
int APS5_VABI sceKernelClose(int);
int APS5_VABI sceKernelGetdents(int, char*, int);
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "Directory symlink check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

int main() {
    namespace fs = std::filesystem;
    // Relative: an absolute path would be read as a guest path.
    const fs::path root("anyps5-directory-symlinks-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(fs::create_directory(root));
    Require(fs::create_directory(root / "target-dir"));
    { std::ofstream stream(root / "target-file"); stream << "x"; }
    const auto links = root / "links";
    Require(fs::create_directory(links));
    fs::create_directory_symlink(fs::absolute(root / "target-dir"), links / "dir-link");
    fs::create_symlink(fs::absolute(root / "target-file"), links / "file-link");
    fs::create_symlink(fs::absolute(root / "missing"), links / "dangling-link");
    Require(fs::create_directory(links / "plain-dir"));

    const int directory = sceKernelOpen(links.string().c_str(), SCE_KERNEL_O_RDONLY | SCE_KERNEL_O_DIRECTORY, 0);
    Require(directory >= 0);
    std::array<char, 1024> buffer{};
    const int read = sceKernelGetdents(directory, buffer.data(), static_cast<int>(buffer.size()));
    Require(read > 0);
    std::map<std::string, int> types;
    for (int offset = 0; offset < read;) {
        std::uint16_t recordLength = 0;
        std::memcpy(&recordLength, buffer.data() + offset + 4, sizeof(recordLength));
        Require(recordLength > 0);
        types[std::string(buffer.data() + offset + 8)] = static_cast<std::uint8_t>(buffer[offset + 6]);
        offset += recordLength;
    }
    Require(sceKernelClose(directory) == 0);

    Require(types.contains("dir-link") && types["dir-link"] == 4);    // DT_DIR
    Require(types.contains("file-link") && types["file-link"] == 8);  // DT_REG
    Require(types.contains("plain-dir") && types["plain-dir"] == 4);
    Require(types.contains("dangling-link") && types["dangling-link"] == 10);  // stays DT_LNK
    fs::remove_all(root);
}

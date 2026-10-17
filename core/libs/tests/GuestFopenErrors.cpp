#include "prx/libc/include/general/VabiMacros.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

extern "C" {
void* APS5_VABI fopen_nid_postfix(const char* filename, const char* mode);
int APS5_VABI fclose_nid_postfix(void* stream);
int* APS5_VABI __error_nid_postfix();
}

namespace {

void Require(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "fopen error check failed at line %d, errno %d\n", line, *__error_nid_postfix());
        std::exit(1);
    }
}
#define Check(value) Require((value), __LINE__)

constexpr int GuestEnoent = 2;
constexpr int GuestEisdir = 21;
constexpr int GuestEloop = 62;
constexpr int GuestEnametoolong = 63;

const std::string LongName(300, 'n');

std::filesystem::path EnterFreshRoot() {
    const auto root = std::filesystem::temp_directory_path() /
        ("anyps5-fopen-errors-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "folder");
    std::filesystem::current_path(root);
    Check(::symlink("loop", "loop") == 0);
    { std::ofstream file("plain.txt"); file << "plain"; }
    return root;
}

bool OpenFails(const char* name, const char* mode, int error) {
    *__error_nid_postfix() = 0;
    const bool failed = fopen_nid_postfix(name, mode) == nullptr && *__error_nid_postfix() == error;
    *__error_nid_postfix() = 0;
    return failed;
}

void Open() {
    Check(OpenFails("loop", "r", GuestEloop));
    Check(OpenFails("loop", "w", GuestEloop));
    Check(OpenFails("loop/inside", "r", GuestEloop));
    Check(OpenFails(LongName.c_str(), "r", GuestEnametoolong));
    Check(OpenFails(LongName.c_str(), "w", GuestEnametoolong));
    Check(OpenFails((LongName + "/inside").c_str(), "a", GuestEnametoolong));
    Check(OpenFails("missing.txt", "r", GuestEnoent));
    Check(OpenFails("folder", "w", GuestEisdir));
    void* plain = fopen_nid_postfix("plain.txt", "r");
    Check(plain != nullptr && fclose_nid_postfix(plain) == 0);
}

}

int main() {
    const auto root = EnterFreshRoot();
    Open();
    std::error_code error;
    std::filesystem::current_path(std::filesystem::temp_directory_path(), error);
    std::filesystem::remove_all(root, error);
    return 0;
}

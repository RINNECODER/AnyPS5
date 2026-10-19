#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

extern "C" {
int APS5_VABI pipe_nid_postfix(int*);
int APS5_VABI dup2_nid_postfix(int, int);
std::int64_t APS5_VABI write_nid_postfix(int, const void*, std::size_t);
}

static void Require(bool value) { if (!value) std::abort(); }

static DWORD Available(int descriptor, void* buffer, DWORD capacity, DWORD& read) {
    const auto handle = reinterpret_cast<HANDLE>(::_get_osfhandle(descriptor));
    DWORD available = 0;
    Require(::PeekNamedPipe(handle, buffer, capacity, &read, &available, nullptr) != 0);
    return available;
}

static void Split(int standard, std::FILE* stream) {
    int capture[2]{-1, -1};
    Require(::_pipe(capture, 65536, _O_BINARY) == 0);
    const int original = ::_dup(standard);
    Require(original >= 0 && ::_dup2(capture[1], standard) == 0);

    int guest[2]{-1, -1};
    Require(pipe_nid_postfix(guest) == 0);
    Require(dup2_nid_postfix(guest[1], standard) == standard);

    std::string host(8192, 'h');
    host.back() = '\n';
    Require(std::fputs(host.c_str(), stream) >= 0);
    Require(std::fflush(stream) == 0);
    Require(write_nid_postfix(standard, "guest", 5) == 5);

    char guestBytes[8]{};
    DWORD guestRead = 0;
    Require(Available(guest[0], guestBytes, sizeof(guestBytes), guestRead) == 5);
    Require(guestRead == 5 && std::string(guestBytes, 5) == "guest");
    char hostBytes[8]{};
    DWORD hostRead = 0;
    Require(Available(capture[0], hostBytes, sizeof(hostBytes), hostRead) == host.size());
    Require(hostRead == sizeof(hostBytes) && std::string(hostBytes, hostRead) == host.substr(0, hostRead));

    Require(::_dup2(original, standard) == 0);
    ::_close(original);
    ::_close(capture[0]);
    ::_close(capture[1]);
    ::_close(guest[0]);
    ::_close(guest[1]);
}

int main() {
    Split(2, stderr);
    Split(1, stdout);
    return 0;
}

#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
int APS5_VABI socket_nid_postfix(int, int, int);
int APS5_VABI bind_nid_postfix(int, const void*, std::uint32_t);
int APS5_VABI listen_nid_postfix(int, int);
int APS5_VABI getsockname_nid_postfix(int, void*, std::uint32_t*);
int APS5_VABI connect_nid_postfix(int, const void*, std::uint32_t);
int APS5_VABI accept_nid_postfix(int, void*, std::uint32_t*);
int APS5_VABI close_nid_postfix(int);
int* APS5_VABI __error_nid_postfix();
}

namespace {

constexpr int Inet = 2;
constexpr int Stream = 1;
constexpr int ErrorNotSupported = 45;

void Require(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "%s (errno %d)\n", what, *__error_nid_postfix());
    std::abort();
}

}

int main() {
    const int listener = socket_nid_postfix(Inet, Stream, 0);
    Require(listener >= 0, "socket failed");
    std::array<std::uint8_t, 16> address{16, Inet, 0, 0, 127, 0, 0, 1};
    Require(bind_nid_postfix(listener, address.data(), address.size()) == 0, "bind failed");
    std::uint32_t length = address.size();
    Require(getsockname_nid_postfix(listener, address.data(), &length) == 0 && length == address.size(), "getsockname failed");
    Require(listen_nid_postfix(listener, 4) == 0, "listen failed");

    *__error_nid_postfix() = 0;
    Require(connect_nid_postfix(listener, address.data(), address.size()) == -1 && *__error_nid_postfix() == ErrorNotSupported,
        "connect on a listening socket must fail with EOPNOTSUPP (45)");

    const int client = socket_nid_postfix(Inet, Stream, 0);
    Require(client >= 0 && connect_nid_postfix(client, address.data(), address.size()) == 0, "a refused connect must leave the listener working");
    const int accepted = accept_nid_postfix(listener, nullptr, nullptr);
    Require(accepted >= 0, "accept failed");
    Require(close_nid_postfix(accepted) == 0 && close_nid_postfix(client) == 0 && close_nid_postfix(listener) == 0, "close failed");
    return 0;
}

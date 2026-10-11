#include "prx/libc/include/general/VabiMacros.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>

extern "C" {
int APS5_VABI setuid_nid_postfix(std::uint32_t);
int APS5_VABI setreuid_nid_postfix(std::uint32_t, std::uint32_t);
int APS5_VABI setgroups_nid_postfix(int, const std::uint32_t*);
int APS5_VABI getgroups_nid_postfix(int, std::uint32_t*);
int APS5_VABI getuid_nid_postfix(void);
int APS5_VABI geteuid_nid_postfix(void);
int APS5_VABI getegid_nid_postfix(void);
int* APS5_VABI __error_nid_postfix();
}

static void Require(bool value) { if (!value) std::abort(); }

constexpr int GuestEfault = 14;
constexpr int GuestEinval = 22;
constexpr std::uint32_t Unchanged = 0xffffffffu;

static void RequireFailure(int result, int error) {
    Require(result == -1);
    Require(*__error_nid_postfix() == error);
}

int main() {
    Require(setuid_nid_postfix(0) == 0);
    Require(getuid_nid_postfix() == 0 && geteuid_nid_postfix() == 0);
    Require(setreuid_nid_postfix(Unchanged, Unchanged) == 0);
    Require(setreuid_nid_postfix(0, Unchanged) == 0);
    Require(setreuid_nid_postfix(Unchanged, 0) == 0);
    Require(setreuid_nid_postfix(0, 0) == 0);

    std::array<std::uint32_t, 4> groups{};
    Require(getgroups_nid_postfix(0, nullptr) == 1);
    Require(getgroups_nid_postfix(static_cast<int>(groups.size()), groups.data()) == 1 && groups[0] == 0);
    Require(groups[0] == static_cast<std::uint32_t>(getegid_nid_postfix()));

    const std::array<std::uint32_t, 3> wanted{0, 5, 7};
    Require(setgroups_nid_postfix(static_cast<int>(wanted.size()), wanted.data()) == 0);
    Require(getgroups_nid_postfix(0, nullptr) == 3);
    RequireFailure(getgroups_nid_postfix(2, groups.data()), GuestEinval);
    RequireFailure(getgroups_nid_postfix(3, nullptr), GuestEfault);
    Require(getgroups_nid_postfix(static_cast<int>(groups.size()), groups.data()) == 3);
    Require(groups[0] == 0 && groups[1] == 5 && groups[2] == 7);

    Require(setgroups_nid_postfix(0, nullptr) == 0);
    Require(getgroups_nid_postfix(static_cast<int>(groups.size()), groups.data()) == 1 && groups[0] == 0);

    RequireFailure(setgroups_nid_postfix(-1, wanted.data()), GuestEinval);
    RequireFailure(setgroups_nid_postfix(1024, wanted.data()), GuestEinval);
    RequireFailure(setgroups_nid_postfix(1, nullptr), GuestEfault);
    Require(getgroups_nid_postfix(0, nullptr) == 1);
    return 0;
}

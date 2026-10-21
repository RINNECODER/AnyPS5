#ifndef CORE_LIBS_PRX_LIBSCECOMMONDIALOG_COMMONDIALOG_HPP
#define CORE_LIBS_PRX_LIBSCECOMMONDIALOG_COMMONDIALOG_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>

struct CommonDialogBaseParam {
    std::uint64_t size;
    std::uint8_t reserved[36];
    std::uint32_t magic;
};

struct NpAuthAuthorizedAppDialogParam {
    CommonDialogBaseParam base;
    std::uint64_t size;
    std::int32_t userId;
    std::uint32_t padding60;
    const char* application;
    const char* text;
    std::uint32_t mode;
    std::uint32_t padding84;
    std::uint8_t reserved[32];
};

static_assert(sizeof(NpAuthAuthorizedAppDialogParam) == 120);
static_assert(offsetof(NpAuthAuthorizedAppDialogParam, application) == 64);
static_assert(offsetof(NpAuthAuthorizedAppDialogParam, text) == 72);
static_assert(offsetof(NpAuthAuthorizedAppDialogParam, mode) == 80);
static_assert(offsetof(NpAuthAuthorizedAppDialogParam, reserved) == 88);

struct VrSetupDialogParam {
    std::uint64_t size;
    std::uint8_t reserved8;
    std::uint8_t reserved9;
    std::uint8_t data[46];
};

static_assert(sizeof(VrSetupDialogParam) == 56);

struct G2PDialogParam {
    CommonDialogBaseParam base;
    std::int32_t userId;
    std::uint32_t padding;
    std::uint64_t size;
    std::uint8_t reserved[64];
};

struct PlayerReviewDialogParam {
    CommonDialogBaseParam base;
    std::uint64_t size;
    std::int32_t userId;
    std::uint8_t target[40];
    std::uint32_t mode;
    std::uint8_t reserved[64];
};

struct CommonDialogResultPrefix {
    std::uint8_t untouched[8];
    std::int32_t result;
};

static_assert(sizeof(CommonDialogBaseParam) == 48);
static_assert(sizeof(G2PDialogParam) == 128);
static_assert(sizeof(PlayerReviewDialogParam) == 168);
static_assert(offsetof(CommonDialogResultPrefix, result) == 8);

inline bool ValidateCommonDialogBaseParam(const CommonDialogBaseParam& param) {
    return param.size == sizeof(param) && param.magic == static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&param)) - 1060003575u && std::all_of(std::begin(param.reserved), std::end(param.reserved), [](std::uint8_t value) { return value == 0; });
}

constexpr int COMMON_DIALOG_OK = 0;
constexpr int COMMON_DIALOG_ERROR_ALREADY_INITIALIZED = static_cast<int>(0x80B80004);

#endif

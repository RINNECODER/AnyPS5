// Guest struct sizes and offsets SceLeafSystemImports hard-codes, asserted against the
// upstream definitions so a layout change upstream fails this build instead of the guest.
#include "SceTypes.hpp"
#include "prx/libSceSaveDataDialog.native/SaveDataDialog.hpp"
#include <cstddef>

static_assert(sizeof(AppContentBootParam) == 40);
static_assert(sizeof(AppContentMountPoint) == 16);
static_assert(sizeof(ImeDialogResult) == 16);
static_assert(offsetof(ImeDialogResult, endstatus) == 0);
static_assert(offsetof(SaveDataDialogParam, mode) == 52);

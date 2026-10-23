# libSceMouse technical debt

### Functional

- [sceMouseResetAbsolutePosition](../../../core/libs/prx/libSceMouse/Export.cpp) (libSceMouse) - the position is not stored because the host reports relative motion only. Native input widths and validation are confirmed, including the default 1920 by 1080 bounds; other native coordinate modes and absolute reports remain unimplemented

# libSceMouse technical debt

### Functional

- [sceMouseResetAbsolutePosition](../../../core/libs/prx/libSceMouse/Export.cpp) (libSceMouse) - the signature and behaviour are not documented on hardware; accepts a device handle matching `sceMouseClose` and `sceMouseRead`, validates initialization and open state, and returns `MOUSE_OK` (0) as the console mouse reports relative motion only

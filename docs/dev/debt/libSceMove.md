# libSceMove technical debt

### Functional


- [libSceMove](../../../core/libs/prx/libSceMove/Export.cpp) (libSceMove) - a handle that was never opened, is closed or was opened before `sceMoveTerm` throws: the console's `INVALID_HANDLE` (`0x80EE0004`) result and its order against `INVALID_ARG` are not known. `sceMoveOpen` throws for a user other than the initial user and for a type and index that are already open, whose console results are not known, and accepts any type and index. Closes when: measured on a PS5
- [libSceMove](../../../core/libs/prx/libSceMove/Export.cpp): the host implements only disconnected controllers. The native module confirms initialization errors, disconnected return 1 without output writes, invalid-handle error 0x80EE0004, and initialization then argument then handle validation for queries. Reset-light-sphere still omits the device ioctl and its failure mapping.
- [sceMoveOpen and sceMoveClose](../../../core/libs/prx/libSceMove/Export.cpp): protected bodies remain unchanged. Native opening resolves a user/device through the input service, rejects duplicate device handles with 0x80EE0007, limits storage to eight slots with 0x80EE0005, and obtains handles from the device ioctl. The host generates monotonically increasing handles, accepts arbitrary type/index values and returns 0x80EE0007 for duplicate slots and throws for other users. Closing an unknown handle returns the confirmed 0x80EE0004.
- [sceMoveReadStateRecent](../../../core/libs/prx/libSceMove/Export.cpp): the native module permits a null data array for SDK versions through 0x03ffffff while still requiring a count pointer and nonnegative timestamp. The host requires the data array for every SDK version. Connected state records and device-service failures remain unimplemented.

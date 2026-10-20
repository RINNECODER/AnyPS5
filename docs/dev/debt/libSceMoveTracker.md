# libSceMoveTracker technical debt

### Unknown function info

- [libSceMoveTracker](../../../core/libs/prx/libSceMoveTracker/Export.cpp) remains unimplemented. Calibration direction, camera/controller input, capture initialization, state query, memory-size query and playback output signatures are recovered. Camera and controller records occupy 24 bytes, state records 160 bytes, and playback metadata 16 bytes; their complete field meanings still require reconstruction. Initialization arguments and capture/playback helper signatures need further checking against caller register use.

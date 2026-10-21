# libSceGameLiveStreaming technical debt

### Functional

- [sceGameLiveStreamingGetProgramInfo](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - the PS5 12.00 module clears the 0x134C-byte program info, asks the system service for its first 0xFD4 bytes and returns `0x80A00002` for a null pointer and `0x80A00004` before initialization. With nothing broadcast here the info stays zero, which assumes the service reports an empty program when nothing is on air; the field layout is unknown. Initialization is not tracked, so `0x80A00004` is never returned

### Unknown function info

- [sceGameLiveStreamingGetCurrentStatus2](../../../core/libs/prx/libSceGameLiveStreaming/Export.cpp) (libSceGameLiveStreaming) - the status layout (user id, on-air flag, spectator, text and command message counts, broadcast resolution, 48 reserved bytes) is taken from shadPS4's `OrbisGameLiveStreamingStatus2`; the call reports that no stream is on air, with an invalid user id

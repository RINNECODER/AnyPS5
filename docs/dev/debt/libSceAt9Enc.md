# libSceAt9Enc technical debt

### Not implemented

- [libSceAt9Enc](../../../core/libs/prx/libSceAt9Enc/Export.cpp) (PS5 system library; name suggests ATRAC9 encoder but unconfirmed; sysmodule id `0x000000d1`) is not implemented in AnyPS5. Only its entry in the sysmodule table exists; there is no `core/libs/prx/libSceAt9Enc` implementation and no host `.prx` is shipped. Titles that link it directly (observed with PPSA03332) fail to load with `libSceAt9Enc.prx: cannot open shared object file`.

### Related unimplemented system libraries

The same load-time failure affects other firmware system libraries that are listed in the sysmodule table but have no implementation or shipped host `.prx` in AnyPS5, observed while running PPSA03332: `libSceAudiodecCpu`, `libSceAudiodecCpuHevag`, `libSceM4aacEnc`, `libSceSulpha`, `libSceCamera`, `libSceDepth2`.
## Signatures (orbital db_types.json)

- `int32_t sceAt9EncClearContext(void *pAt9EncWorkArea, int32_t *pInternalResult)`
- `int32_t sceAt9EncCreateEncoder(void *pAt9EncWorkArea, struct *pAt9EncParam, int32_t memSize, int32_t *pInternalResult)`
- `int32_t sceAt9EncEncode(void *pAt9EncWorkArea, const void *pInputBuf, void *pOutputBuf, uint32_t inputBufSize, uint32_t outputBufSize, uint32_t *pOutputDataByte, uint32_t *pUsedBufSize, int32_t *pInternalResult)`
- `int32_t sceAt9EncFlush(void *pAt9EncWorkArea, void *pOutputBuf, uint32_t outputBufSize, uint32_t *pOutputDataByte, int32_t *pInternalResult)`
- `int32_t sceAt9EncQueryMemSize(struct *pAt9EncParam, int32_t *pMemSize, int32_t *pInternalResult)`
